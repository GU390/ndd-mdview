// ============================================================================
//  ndd-mdview.cpp
//  Notepad-- 插件：内嵌 Microsoft Edge WebView2 的 Markdown 实时预览
//
//  设计要点
//  --------
//  1) 宿主 ABI（src/pluginGl.h，与 Notepad-- v3.8.3 一致）：
//       NDD_PROC_IDENTIFY : 扫描 plugin\*.dll 时调用，填名字/说明/版本
//       NDD_PROC_MAIN     : 宿主建好一级 QAction 后调用，插件自己连 triggered
//     宿主在 m_menuType==0 时会建 QAction 并塞进 pProcData->m_pAction；
//     宿主那个 connect(pAction, triggered, onPlugWork) 在源码里是被注释掉的，
//     所以必须由插件自己连。
//
//  2) 取编辑区文本 **不链接 qscint**：
//     宿主把 qscint 静态链进 qmyedit_qt5.dll，符号不导出；而且 QsciScintilla
//     的类布局跨版本会变，跨模块 dynamic_cast/qobject_cast 不可靠。
//     改走纯 Win32：枚举主窗口子孙窗口，认类名 "ScintillaEditView"（Scintilla
//     原生窗口，QsciScintilla 的子窗口），用 SCI_GETLENGTH / SCI_GETTEXT /
//     SCI_GETCODEPAGE 直接取。零 ABI 风险，只依赖 Win32 消息。
//
//  3) 文本传输用 base64：避免 JSON/JS 字符串转义地狱；JS 侧 TextDecoder 解码。
//
//  4) 面板用 QDockWidget 挂在主窗口右侧；宿主主窗口是 QMainWindow
//     （CCNotePad : public QMainWindow），qobject_cast 判定。
//     退化路径：不是 QMainWindow 时退化成独立顶层窗口。
//
//  5) 本文件不含 Q_OBJECT，Qt 的 moc 不需要跑。
// ============================================================================

#include <QtGlobal>
#include <QWidget>
#include <QObject>
#include <QAction>
#include <QMenu>
#include <QTimer>
#include <QString>
#include <QByteArray>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QMainWindow>
#include <QDockWidget>
#include <QPointer>
#include <QShowEvent>
#include <QHideEvent>
#include <QResizeEvent>
#include <QDebug>

#include <functional>
#include <vector>

#include <windows.h>
#include <wrl/client.h>
#include <wrl/implements.h>   // Microsoft::WRL::Callback 在这个头里

#include "pluginGl.h"
#include "WebView2.h"

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
//  宿主 ABI 导出
// ---------------------------------------------------------------------------
#define NDD_EXPORTDLL
#if defined(Q_OS_WIN)
#  if defined(NDD_EXPORTDLL)
#    define NDD_EXPORT __declspec(dllexport)
#  else
#    define NDD_EXPORT __declspec(dllimport)
#  endif
#else
#  define NDD_EXPORT __attribute__((visibility("default")))
#endif

// ABI 要求的前置声明。
// 宿主传的是 std::function<QsciScintilla*(QWidget*)>，我们并不使用它
//（见文件头第 2 点），但函数签名必须一字不差，所以这里只做前置声明。
class QsciScintilla;

// ---------------------------------------------------------------------------
//  Scintilla 消息号（公开 API，跨版本稳定）
// ---------------------------------------------------------------------------
#define SCI_GETLENGTH      2006
#define SCI_GETTEXT        2182
#define SCI_GETCODEPAGE    2137

// 轮询间隔（毫秒）
static const int kPollIntervalMs = 250;

// markdown-it 资源路径（见 resources.qrc）
static const char* kMarkdownItResource = ":/mdview/markdown-it.min.js";

// ===========================================================================
//  小工具
// ===========================================================================

// 在 root 的全部子孙窗口里找 Scintilla 原生编辑窗口。
// 优先取「可见」的那个（QTabWidget 里非当前页是隐藏的），
// 可见者多个时取面积最大的。
static HWND FindScintillaEdit(HWND root)
{
    struct Ctx { std::vector<HWND> hits; };
    Ctx ctx;

    // EnumChildWindows 本身就会递归枚举所有子孙。
    EnumChildWindows(root, [](HWND hwnd, LPARAM lp) -> BOOL {
        Ctx* c = reinterpret_cast<Ctx*>(lp);
        wchar_t cls[256];
        cls[0] = L'\0';
        GetClassNameW(hwnd, cls, 256);
        if (wcscmp(cls, L"ScintillaEditView") == 0) {
            c->hits.push_back(hwnd);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    if (ctx.hits.empty())
        return nullptr;

    HWND best = nullptr;
    long bestArea = -1;
    for (size_t i = 0; i < ctx.hits.size(); ++i) {
        HWND h = ctx.hits[i];
        if (!IsWindowVisible(h))
            continue;
        RECT rc;
        GetClientRect(h, &rc);
        long area = (long)(rc.right - rc.left) * (long)(rc.bottom - rc.top);
        if (area > bestArea) {
            bestArea = area;
            best = h;
        }
    }
    // 全都不可见（例如当前不是文本编辑态）时，退回第一个
    return best ? best : ctx.hits.front();
}

// 按 Scintilla 文档编码把原始字节解成 QString
static QString DecodeDocBytes(const std::vector<char>& buf, int len, int codePage)
{
    if (len <= 0)
        return QString();

    if (codePage == 0 || codePage == 65001) {          // 0 / CP_UTF8
        return QString::fromUtf8(buf.data(), len);
    }
    if (codePage == 1200 || codePage == 1201) {         // UTF-16 LE / BE
        return QString::fromUtf16(reinterpret_cast<const ushort*>(buf.data()), len / 2);
    }
    int wlen = MultiByteToWideChar(codePage, 0, buf.data(), len, nullptr, 0);
    if (wlen <= 0)
        return QString::fromUtf8(buf.data(), len);
    std::vector<wchar_t> wide((size_t)wlen + 1, 0);
    MultiByteToWideChar(codePage, 0, buf.data(), len, wide.data(), wlen);
    return QString::fromWCharArray(wide.data(), wlen);
}

// 读取 Scintilla 里的全文。失败返回 false。
static bool ReadScintillaText(HWND hwnd, QString& out)
{
    if (hwnd == nullptr)
        return false;

    LRESULT len = SendMessageW(hwnd, SCI_GETLENGTH, 0, 0);
    if (len < 0)
        return false;
    if (len == 0) {
        out.clear();
        return true;
    }

    std::vector<char> buf((size_t)len + 1, 0);
    SendMessageW(hwnd, SCI_GETTEXT, (WPARAM)(len + 1), (LPARAM)buf.data());

    int cp = (int)SendMessageW(hwnd, SCI_GETCODEPAGE, 0, 0);
    out = DecodeDocBytes(buf, (int)len, cp);
    return true;
}

// ===========================================================================
//  预览面板（QWidget 子类，无 Q_OBJECT，不需要 moc）
// ===========================================================================
class MdViewPanel : public QWidget
{
public:
    explicit MdViewPanel(QWidget* parent = nullptr)
        : QWidget(parent)
    {
    }

    ~MdViewPanel() override
    {
        if (m_poll) {
            m_poll->stop();
        }
    }

    void setHostWidget(QWidget* w) { m_host = w; }

    // 初始化（异步创建 WebView2）。返回 false 表示立即失败。
    bool start(const QString& pluginDir);

protected:
    void resizeEvent(QResizeEvent* e) override
    {
        QWidget::resizeEvent(e);
        syncBounds();
    }

    void showEvent(QShowEvent* e) override
    {
        QWidget::showEvent(e);

        // 停靠位置变化 / 浮动 / 还原 时，Qt 可能重建原生窗口，
        // 那样 WebView2 的父句柄就失效了 —— 检测到变化就重建控制器。
        HWND cur = reinterpret_cast<HWND>(winId());
        if (m_view && cur != m_hwnd) {
            m_hwnd = cur;
            m_ctrl.Reset();
            m_view.Reset();
            m_pageReady = false;
            createController();
        }

        if (m_poll)
            m_poll->start();
        pollNow();
    }

    void hideEvent(QHideEvent* e) override
    {
        if (m_poll)
            m_poll->stop();
        QWidget::hideEvent(e);
    }

private:
    void syncBounds()
    {
        if (!m_ctrl)
            return;
        RECT r;
        r.left = 0;
        r.top = 0;
        r.right = width();
        r.bottom = height();
        m_ctrl->put_Bounds(r);
    }

    void createController();
    QString buildHtml(const QString& mdJs) const;
    void navigateToApp();
    void pollNow();
    void showError(const QString& msg);

    QPointer<QWidget> m_host;
    HWND m_hwnd = nullptr;

    ComPtr<ICoreWebView2Environment> m_env;
    ComPtr<ICoreWebView2Controller> m_ctrl;
    ComPtr<ICoreWebView2> m_view;

    QTimer* m_poll = nullptr;
    QString m_currentText;
    QByteArray m_lastPayload;
    bool m_pageReady = false;
};

// --- 面板实现 ---------------------------------------------------------------

QString MdViewPanel::buildHtml(const QString& mdJs) const
{
    // 注意：markdown-it.min.js 直接内联进 <script>，
    // 官方构建里不含 "</script>" 字面量，安全。
    static const char* kCss =
        "html,body{margin:0;padding:0;}"
        "body{font-family:'Microsoft YaHei','Segoe UI',system-ui,sans-serif;"
        "font-size:14px;line-height:1.7;color:#24292f;background:#ffffff;}"
        "#md{max-width:900px;margin:0 auto;padding:20px 24px 60px 24px;"
        "box-sizing:border-box;word-wrap:break-word;}"
        "#md h1,#md h2,#md h3,#md h4,#md h5,#md h6{margin:1.2em 0 .6em 0;"
        "font-weight:600;line-height:1.3;}"
        "#md h1{font-size:1.9em;border-bottom:1px solid #d8dee4;padding-bottom:.3em;}"
        "#md h2{font-size:1.5em;border-bottom:1px solid #d8dee4;padding-bottom:.3em;}"
        "#md h3{font-size:1.25em;} #md h4{font-size:1.05em;}"
        "#md p{margin:.6em 0;}"
        "#md a{color:#0969da;text-decoration:none;} #md a:hover{text-decoration:underline;}"
        "#md code{background:#eff1f3;padding:.15em .35em;border-radius:4px;"
        "font-family:Consolas,'Courier New',monospace;font-size:.92em;}"
        "#md pre{background:#f6f8fa;padding:14px 16px;border-radius:6px;overflow:auto;"
        "border:1px solid #e5e7eb;}"
        "#md pre code{background:none;padding:0;font-size:.9em;line-height:1.5;}"
        "#md blockquote{margin:.8em 0;padding:.2em 1em;color:#57606a;"
        "border-left:4px solid #d0d7de;background:#f8f9fb;}"
        "#md table{border-collapse:collapse;margin:1em 0;display:block;overflow:auto;}"
        "#md th,#md td{border:1px solid #d0d7de;padding:6px 13px;}"
        "#md th{background:#f6f8fa;font-weight:600;}"
        "#md ul,#md ol{padding-left:2em;margin:.6em 0;}"
        "#md li{margin:.2em 0;}"
        "#md img{max-width:100%;}"
        "#md hr{border:none;border-top:1px solid #d8dee4;margin:1.6em 0;}";

    // JS 侧：window.mdUpdate(base64) -> 解码 -> 渲染
    static const char* kApi =
        "(function(){"
        "  var md = window.markdownit({html:false,linkify:true,breaks:false,"
        "typographer:false,xhtmlOut:false});"
        "  window.mdUpdate = function(b64){"
        "    try{"
        "      var bin = atob(b64);"
        "      var bytes = new Uint8Array(bin.length);"
        "      for(var i=0;i<bin.length;i++){bytes[i]=bin.charCodeAt(i);}"
        "      var text = new TextDecoder('utf-8').decode(bytes);"
        "      document.getElementById('md').innerHTML = md.render(text);"
        "    }catch(e){"
        "      document.getElementById('md').textContent = 'Render error: '+e;"
        "    }"
        "  };"
        "})();";

    QString html;
    html.reserve(mdJs.size() + 8192);
    html += QStringLiteral("<!DOCTYPE html><html><head><meta charset=\"utf-8\">");
    html += QStringLiteral("<title>Markdown Preview</title>");
    html += QStringLiteral("<style>");
    html += QString::fromUtf8(kCss);
    html += QStringLiteral("</style></head><body>");
    html += QStringLiteral("<div id=\"md\"></div>");
    html += QStringLiteral("<script>");
    html += mdJs;
    html += QStringLiteral("</script><script>");
    html += QString::fromUtf8(kApi);
    html += QStringLiteral("</script></body></html>");
    return html;
}

bool MdViewPanel::start(const QString& pluginDir)
{
    setAttribute(Qt::WA_NativeWindow, true);
    m_hwnd = reinterpret_cast<HWND>(winId());
    if (m_hwnd == nullptr) {
        showError(QStringLiteral("无法创建原生窗口 (winId 失败)"));
        return false;
    }

    // WebView2Loader.dll：优先从插件目录找，找不到再走系统搜索
    HMODULE hLoader = nullptr;
    const QString loaderPath = QDir(pluginDir).filePath(QStringLiteral("WebView2Loader.dll"));
    if (!loaderPath.isEmpty()) {
        hLoader = LoadLibraryExW(reinterpret_cast<LPCWSTR>(loaderPath.utf16()), nullptr,
                                 LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    if (hLoader == nullptr) {
        hLoader = LoadLibraryW(L"WebView2Loader.dll");
    }
    if (hLoader == nullptr) {
        showError(QStringLiteral("WebView2Loader.dll 加载失败\r\n"
                                 "请确认它和本插件 DLL 放在同一目录，"
                                 "或已安装 Microsoft Edge WebView2 Runtime。"));
        return false;
    }

    typedef HRESULT(WINAPI* PFN_CreateEnvWithOptions)(
        PCWSTR browserExecutableFolder,
        PCWSTR userDataFolder,
        ICoreWebView2EnvironmentOptions* environmentOptions,
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* environmentCreatedHandler);

    PFN_CreateEnvWithOptions fn =
        reinterpret_cast<PFN_CreateEnvWithOptions>(
            GetProcAddress(hLoader, "CreateCoreWebView2EnvironmentWithOptions"));
    if (fn == nullptr) {
        showError(QStringLiteral("WebView2Loader.dll 缺少 "
                                 "CreateCoreWebView2EnvironmentWithOptions 入口"));
        return false;
    }

    QPointer<MdViewPanel> self(this);
    HRESULT hr = fn(nullptr, nullptr, nullptr,
                    Microsoft::WRL::Callback<
                        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                        [self](HRESULT code, ICoreWebView2Environment* env) -> HRESULT {
                            if (!self)
                                return S_OK;
                            if (FAILED(code) || env == nullptr) {
                                self->showError(QStringLiteral("创建 WebView2 环境失败 (hr=0x%1)")
                                                    .arg((quint32)code, 8, 16, QLatin1Char('0')));
                                return S_OK;
                            }
                            self->m_env = env;
                            self->createController();
                            return S_OK;
                        })
                        .Get());
    (void)hr;

    // 轮询器：只在面板可见时跑
    m_poll = new QTimer(this);
    m_poll->setInterval(kPollIntervalMs);
    QObject::connect(m_poll, &QTimer::timeout, this, [this]() { pollNow(); });

    return true;
}

void MdViewPanel::createController()
{
    if (!m_env || m_hwnd == nullptr)
        return;

    QPointer<MdViewPanel> self(this);
    m_env->CreateCoreWebView2Controller(
        m_hwnd,
        Microsoft::WRL::Callback<
            ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [self](HRESULT code, ICoreWebView2Controller* ctrl) -> HRESULT {
                if (!self)
                    return S_OK;
                if (FAILED(code) || ctrl == nullptr) {
                    self->showError(QStringLiteral("创建 WebView2 控制器失败 (hr=0x%1)")
                                        .arg((quint32)code, 8, 16, QLatin1Char('0')));
                    return S_OK;
                }
                self->m_ctrl = ctrl;

                ComPtr<ICoreWebView2> view;
                if (FAILED(self->m_ctrl->get_CoreWebView2(&view)) || view == nullptr) {
                    self->showError(QStringLiteral("取得 WebView2 实例失败"));
                    return S_OK;
                }
                self->m_view = view;
                self->syncBounds();
                self->navigateToApp();
                return S_OK;
            })
            .Get());
}

void MdViewPanel::navigateToApp()
{
    if (!m_view)
        return;

    QFile f(QString::fromUtf8(kMarkdownItResource));
    QString mdJs;
    if (f.open(QIODevice::ReadOnly)) {
        mdJs = QString::fromUtf8(f.readAll());
    } else {
        mdJs = QStringLiteral("/* markdown-it 资源缺失 */");
    }

    const QString html = buildHtml(mdJs);

    QPointer<MdViewPanel> self(this);
    EventRegistrationToken token;
    HRESULT hr = m_view->add_NavigationCompleted(
        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [self](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                if (!self)
                    return S_OK;
                self->m_pageReady = true;
                self->pushCurrentText();
                return S_OK;
            })
            .Get(),
        &token);
    (void)hr;

    m_view->NavigateToString(reinterpret_cast<LPCWSTR>(html.utf16()));
}

void MdViewPanel::pushCurrentText()
{
    if (!m_view || !m_pageReady)
        return;

    const QByteArray payload = m_currentText.toUtf8().toBase64();
    if (payload == m_lastPayload)
        return;
    m_lastPayload = payload;

    const QString js = QStringLiteral("window.mdUpdate('")
        + QString::fromLatin1(payload) + QStringLiteral("')");
    m_view->ExecuteScript(reinterpret_cast<LPCWSTR>(js.utf16()));
}

void MdViewPanel::pollNow()
{
    if (!m_host)
        return;

    HWND sci = FindScintillaEdit(reinterpret_cast<HWND>(m_host->winId()));
    if (sci == nullptr)
        return;

    QString text;
    if (!ReadScintillaText(sci, text))
        return;
    if (text == m_currentText)
        return;

    m_currentText = text;
    pushCurrentText();
}

void MdViewPanel::showError(const QString& msg)
{
    qWarning() << "[ndd-mdview]" << msg;

    const QString html = QStringLiteral(
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><style>"
        "body{font-family:'Microsoft YaHei',sans-serif;padding:24px;color:#b42318;"
        "background:#fff8f8;line-height:1.8;}"
        "h3{margin:0 0 10px 0;font-size:16px;}"
        "pre{white-space:pre-wrap;font-family:Consolas,monospace;font-size:13px;"
        "background:#fff;border:1px solid #f0c0c0;padding:10px;border-radius:6px;}"
        "</style></head><body><h3>Markdown 预览启动失败</h3><pre>")
        + msg.toHtmlEscaped() + QStringLiteral("</pre></body></html>");

    if (m_view) {
        m_view->NavigateToString(reinterpret_cast<LPCWSTR>(html.utf16()));
    } else {
        setWindowTitle(QStringLiteral("Markdown 预览启动失败"));
        setToolTip(msg);
    }
}

// ===========================================================================
//  控制器：管停靠窗口 + 面板 + 菜单动作
// ===========================================================================
class MdController : public QObject
{
public:
    explicit MdController(QObject* parent = nullptr) : QObject(parent) {}

    QPointer<QWidget> host;
    QPointer<QDockWidget> dock;
    MdViewPanel* panel = nullptr;
    QString pluginDir;

    void ensure()
    {
        if (dock || panel)
            return;

        QMainWindow* mw = host ? qobject_cast<QMainWindow*>(host.data()) : nullptr;
        if (mw) {
            dock = new QDockWidget(QStringLiteral("Markdown 实时预览"), mw);
            dock->setObjectName(QStringLiteral("ndd_mdview_dock"));
            panel = new MdViewPanel(dock);
            panel->setHostWidget(host);
            // setWidget 会把 panel 重新挂到 dock 内部容器上，
            // 必须在它之后才 winId()，否则原生窗口会被重建（WebView2 父句柄失效）。
            dock->setWidget(panel);
            mw->addDockWidget(Qt::RightDockWidgetArea, dock);
            panel->start(pluginDir);
            dock->hide();
        } else {
            // 退化：不是 QMainWindow 就开独立顶层窗口
            panel = new MdViewPanel(nullptr);
            panel->setWindowFlags(Qt::Window);
            panel->setWindowTitle(QStringLiteral("Markdown 实时预览"));
            panel->resize(760, 900);
            panel->setHostWidget(host);
            panel->start(pluginDir);
            panel->show();
        }
    }

    void toggle()
    {
        ensure();
        if (dock) {
            if (dock->isVisible()) {
                dock->hide();
            } else {
                dock->show();
                dock->raise();
            }
        } else if (panel) {
            panel->setVisible(!panel->isVisible());
            panel->raise();
        }
    }
};

// ===========================================================================
//  宿主 ABI 导出
// ===========================================================================
static MdController* g_controller = nullptr;

extern "C" {

NDD_EXPORT bool NDD_PROC_IDENTIFY(NDD_PROC_DATA* pProcData)
{
    if (pProcData == nullptr)
        return false;

    pProcData->m_strPlugName = QString::fromUtf8(u8"Markdown 实时预览 (WebView2)");
    pProcData->m_strComment = QString::fromUtf8(u8"在编辑区右侧实时渲染 Markdown，"
                                                u8"基于 Microsoft Edge WebView2");
    pProcData->m_version = QString::fromUtf8(u8"v1.0");
    pProcData->m_auther = QString::fromUtf8(u8"ndd-mdview");
    pProcData->m_menuType = 0;   // 一级菜单，宿主建 QAction
    return true;
}

NDD_EXPORT int NDD_PROC_MAIN(QWidget* pNotepad,
                              const QString& strFileName,
                              std::function<QsciScintilla*(QWidget*)> getCurEdit,
                              std::function<bool(QWidget*, int, void*)> pluginCallBack,
                              NDD_PROC_DATA* pProcData)
{
    Q_UNUSED(getCurEdit);
    Q_UNUSED(pluginCallBack);

    // m_menuType==0 的正常路径下 pProcData 有效（含 m_pAction）。
    // 另一条路径（宿主 onPlugWork）传的是 nullptr，那种情况没法挂动作，直接退出。
    if (pProcData == nullptr || pProcData->m_pAction == nullptr)
        return -1;

    MdController* ctl = new MdController(pNotepad);
    ctl->host = pNotepad;
    ctl->pluginDir = QFileInfo(strFileName).absolutePath();
    g_controller = ctl;

    QObject::connect(pProcData->m_pAction, &QAction::triggered, ctl,
                     [ctl]() { ctl->toggle(); }, Qt::UniqueConnection);

    return 0;
}

}  // extern "C"

// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD fdwReason, LPVOID lpReserved)
{
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
        break;
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        if (NULL == lpReserved) {
            // 全局回收
        }
        break;
    }
    return TRUE;
}
