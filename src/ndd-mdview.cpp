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
//  2) 取编辑区文本走 **宿主导出的 QsciScintilla::text()**：
//     - 宿主用 QScintilla，它的 Scintilla 那一侧是 QWidget（ScintillaQt），
//       **没有**原生 "Scintilla" 窗口类（实测枚举主窗口全部子孙，只有
//       Qt5152QWindowIcon 与 Chrome_*），SCI_GETTEXT 这类消息根本发不出去；
//     - qscintilla 的符号宿主**全导出了**（qmyedit_qt5.dll 导出表 2403 项，
//       含 ?text@QsciScintilla@@QEBA?AVQString@@XZ），宿主自己的 Markdown
//       预览用的也是 pEditView->text()；
//     - 于是只声明一个同名同签名的最小类 + GetProcAddress 拿真实地址，
//       不链接 qscintilla、也不依赖它的类布局。详见下面「取文工具层」。
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
#include <QApplication>
#include <QAction>
#include <QMenu>
#include <QTimer>
#include <QString>
#include <QStringList>
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
#include <objbase.h>        // CoInitializeEx / CoUninitialize
#include <wrl/client.h>     // Microsoft::WRL::ComPtr

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

// ---------------------------------------------------------------------------
//  取文工具层：调宿主导出的 QsciScintilla::text()
//
//  为什么需要这么绕：
//    宿主把 qscintilla 静态链进 qmyedit_qt5.dll，头文件不随插件分发，
//    QsciScintilla 的完整类布局又跨版本会变 —— 直接 #include <qsciscintilla.h>
//    并链接是不可行的。
//
//  但符号是导出来的（实测 qmyedit_qt5.dll 导出表 2403 项），
//  其中 ?text@QsciScintilla@@QEBA?AVQString@@XZ 就是
//  `QString QsciScintilla::text() const` —— 宿主自己的 Markdown 预览
//  （CCNotePad::slot_markdownPreview）用的也正是 pEditView->text()。
//
//  所以这里只声明一个「同名同签名的最小类」，函数指针由 GetProcAddress
//  取得。text() 是**非虚**成员函数，调用时编译器只把 this 原样传下去，
//  不需要知道真实类的布局 —— 我们也从不 new/delete 它。
// ---------------------------------------------------------------------------

// ABI 签名要求 std::function<QsciScintilla*(QWidget*)>，这里的名字必须
// 与宿主一致（模板实参只用到指针类型，名字不影响 ABI）。
class QsciScintilla
{
public:
    QString text() const;
};

// MSVC x64：类类型按值返回走隐藏返回缓冲区（sret，RCX），
// 第一个显式/隐式实参进 RDX。
//
// 实测（dump qmyedit_qt5.dll 的机器码确认）：
//   ?text@QsciScintilla@@QEBA?AVQString@@XZ
//     +1a  4c 8b f2   mov r14, rdx   ← this = RDX
//     +29  48 8b e9   mov rbp, rcx   ← sret = RCX
//
// 所以这里声明成「自己带返回缓冲区」的形态：寄存器布局跟
// `QString (*)(QsciScintilla*)` 完全一致（RCX=缓冲区，RDX=this），
// 但返回缓冲区由调用方准备 —— 这样 __try 里就没有需要回卷的 C++ 对象
// （否则 MSVC 报 C2712）。
typedef void (*SciTextFn)(QString* ret, QsciScintilla* self);

// 宿主 qscintilla 那个 DLL 里 text() 的修饰名
static const char* kSciTextSymbol = "?text@QsciScintilla@@QEBA?AVQString@@XZ";

// 在已加载模块里找导出 QsciScintilla::text() 的那个 DLL，返回其函数地址
static SciTextFn ResolveSciTextFn()
{
    // 1) 已知模块名：不同宿主版本可能换名字，都试一遍
    static const char* kCandidates[] = {
        "qmyedit_qt5.dll",
        "qmyedit_qt6.dll",
        "qmyedit.dll",
        "qscintilla.dll",
    };
    for (size_t i = 0; i < sizeof(kCandidates) / sizeof(kCandidates[0]); ++i) {
        HMODULE h = GetModuleHandleA(kCandidates[i]);
        if (h) {
            FARPROC p = GetProcAddress(h, kSciTextSymbol);
            if (p)
                return reinterpret_cast<SciTextFn>(p);
        }
    }

    // 2) 兜底：遍历本进程已加载的所有模块
    typedef BOOL(WINAPI *EnumProcessModulesFn)(HANDLE, HMODULE*, DWORD, DWORD*);
    EnumProcessModulesFn enumMods = nullptr;
    HMODULE psapi = LoadLibraryW(L"psapi.dll");
    if (psapi)
        enumMods = reinterpret_cast<EnumProcessModulesFn>(GetProcAddress(psapi, "EnumProcessModules"));
    if (!enumMods) {
        // kernel32 里是 K32EnumProcessModules
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (k32)
            enumMods = reinterpret_cast<EnumProcessModulesFn>(GetProcAddress(k32, "K32EnumProcessModules"));
    }
    if (enumMods) {
        HMODULE mods[1024];
        DWORD needed = 0;
        if (enumMods(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
            DWORD count = needed / sizeof(HMODULE);
            for (DWORD i = 0; i < count; ++i) {
                FARPROC p = GetProcAddress(mods[i], kSciTextSymbol);
                if (p) {
                    if (psapi)
                        FreeLibrary(psapi);
                    return reinterpret_cast<SciTextFn>(p);
                }
            }
        }
    }
    if (psapi)
        FreeLibrary(psapi);
    return nullptr;
}

// ---------------------------------------------------------------------------
//  定位当前编辑区
//
//  为什么不直接用宿主给的 getCurEdit 仿函数：
//    实测（v1.0 第一版）用它拿到的指针是垃圾，调用 text() 直接在
//    qmyedit_qt5.dll 的 SendScintilla 里访问违例（0xc0000005，崩宿主）。
//    寄存器约定已用机器码核对过是对的（sret=RCX / this=RDX），
//    所以问题出在那个指针本身 —— 跨模块 std::function 的可靠性存疑，
//    不敢再依赖。
//
//  改用 Qt 自己的对象树定位：宿主主窗口 → findChildren<QWidget*>() →
//  用 metaObject()->className() 认 "ScintillaEditView"。
//  这条路全是 Qt 自己的 API（metaObject 是虚函数，vtable 在宿主 EXE 里，
//  调用完全安全）。
//
//  关键前提：ScintillaEditView : QsciScintilla : QsciScintillaBase
//  : QAbstractScrollArea : QFrame : QWidget : QObject —— 全是单继承，
//  所以 **所有基类子对象的偏移都是 0**：
//  QWidget* == QsciScintilla* == 同一个地址。
//  于是拿 QWidget* 直接当 QsciScintilla* 传给 text() 是对的。
// ---------------------------------------------------------------------------
static QWidget* FindCurrentEditor(QWidget* host)
{
    if (host == nullptr)
        return nullptr;

    QWidget* focus = QApplication::focusWidget();
    QWidget* best = nullptr;
    long bestArea = -1;

    const QList<QWidget*> kids = host->findChildren<QWidget*>();
    for (int i = 0; i < kids.size(); ++i) {
        QWidget* w = kids.at(i);
        const QMetaObject* mo = w->metaObject();
        if (mo == nullptr)
            continue;
        if (qstrcmp(mo->className(), "ScintillaEditView") != 0)
            continue;
        if (!w->isVisible())
            continue;
        if (w == focus)
            return w;                       // 有焦点的优先
        const QRect r = w->rect();
        const long area = (long)r.width() * (long)r.height();
        if (area > bestArea) {
            bestArea = area;
            best = w;
        }
    }
    return best;
}

// 调 text() 时兜一层 SEH：万一指针不对，别把宿主带崩。
// 函数里不放任何需要回卷的 C++ 对象（否则 MSVC 会报 C2712）：
// 返回缓冲区由调用方（pollNow）准备好，这里只传指针。
static bool SafeSciText(SciTextFn fn, QsciScintilla* self, QString* out)
{
    __try {
        fn(out, self);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 轮询间隔（毫秒）
static const int kPollIntervalMs = 250;

// markdown-it 资源路径（见 resources.qrc）
static const char* kMarkdownItResource = ":/mdview/markdown-it.min.js";

// ===========================================================================
//  手写 COM 回调对象
//
//  Windows SDK 的 wrl（winrt\wrl 那套）里只有 ComPtr，没有
//  Microsoft::WRL::Callback；而 WebView2 的完成回调接口都是
//  「IUnknown 派生 + 一个 Invoke」，所以自己实现一个带引用计数的
//  最小 COM 对象就够了，也不必赌 SDK 里到底哪个头提供 Callback。
// ===========================================================================
class EnvCompletedHandler final
    : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
{
public:
    typedef std::function<void(HRESULT, ICoreWebView2Environment*)> Fn;

    explicit EnvCompletedHandler(Fn fn) : m_fn(std::move(fn)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
            *ppv = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_ref));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = static_cast<ULONG>(InterlockedDecrement(&m_ref));
        if (n == 0)
            delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT code, ICoreWebView2Environment* env) override
    {
        m_fn(code, env);
        return S_OK;
    }

private:
    Fn m_fn;
    long m_ref = 1;
};

class CtrlCompletedHandler final
    : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
{
public:
    typedef std::function<void(HRESULT, ICoreWebView2Controller*)> Fn;

    explicit CtrlCompletedHandler(Fn fn) : m_fn(std::move(fn)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
            *ppv = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_ref));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = static_cast<ULONG>(InterlockedDecrement(&m_ref));
        if (n == 0)
            delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT code, ICoreWebView2Controller* ctrl) override
    {
        m_fn(code, ctrl);
        return S_OK;
    }

private:
    Fn m_fn;
    long m_ref = 1;
};

class NavCompletedHandler final
    : public ICoreWebView2NavigationCompletedEventHandler
{
public:
    typedef std::function<void()> Fn;

    explicit NavCompletedHandler(Fn fn) : m_fn(std::move(fn)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(ICoreWebView2NavigationCompletedEventHandler)) {
            *ppv = static_cast<ICoreWebView2NavigationCompletedEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_ref));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = static_cast<ULONG>(InterlockedDecrement(&m_ref));
        if (n == 0)
            delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) override
    {
        m_fn();
        return S_OK;
    }

private:
    Fn m_fn;
    long m_ref = 1;
};

class ExecScriptHandler final
    : public ICoreWebView2ExecuteScriptCompletedHandler
{
public:
    typedef std::function<void(HRESULT, LPCWSTR)> Fn;

    explicit ExecScriptHandler(Fn fn) : m_fn(std::move(fn)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(ICoreWebView2ExecuteScriptCompletedHandler)) {
            *ppv = static_cast<ICoreWebView2ExecuteScriptCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_ref));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = static_cast<ULONG>(InterlockedDecrement(&m_ref));
        if (n == 0)
            delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT code, LPCWSTR result) override
    {
        m_fn(code, result);
        return S_OK;
    }

private:
    Fn m_fn;
    long m_ref = 1;
};

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
        if (m_comInitialized) {
            CoUninitialize();
        }
    }

    void setHostWidget(QWidget* w) { m_host = w; }
    void setDockWidget(QDockWidget* w) { m_dock = w; }

    // 取文来源：宿主给的「拿当前编辑区」仿函数 + 导出的 text() 入口
    void setTextSource(std::function<QsciScintilla*(QWidget*)> getCurEdit, SciTextFn fn)
    {
        m_getCurEdit = std::move(getCurEdit);
        m_textFn = fn;
    }

    // 状态显示在停靠窗格标题上，方便一眼看出卡在哪一步
    void setStatus(const QString& s)
    {
        m_status = s;
        if (m_dock) {
            m_dock->setWindowTitle(m_status.isEmpty()
                                       ? QStringLiteral("Markdown 实时预览")
                                       : QStringLiteral("Markdown 实时预览 | ") + m_status);
        }
    }

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

        // 布局可能还没稳定，过一会儿再对一次 bounds、并补一次推送
        QTimer::singleShot(0, this, [this]() { syncBounds(); });
        QTimer::singleShot(400, this, [this]() {
            syncBounds();
            m_lastPayload.clear();
            pushCurrentText();
        });
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
        if (r.right <= 0 || r.bottom <= 0)
            return;
        m_ctrl->put_Bounds(r);
    }

    void createController();
    QString buildHtml(const QString& mdJs) const;
    void navigateToApp();
    void pushCurrentText();
    void pollNow();
    void showError(const QString& msg);
    void logChildren() const;

    QPointer<QWidget> m_host;
    QPointer<QDockWidget> m_dock;
    std::function<QsciScintilla*(QWidget*)> m_getCurEdit;
    SciTextFn m_textFn = nullptr;
    QWidget* m_lastEditor = nullptr;
    HWND m_hwnd = nullptr;
    QString m_pluginDir;
    HRESULT m_coInitResult = S_OK;
    bool m_comInitialized = false;

    ComPtr<ICoreWebView2Environment> m_env;
    ComPtr<ICoreWebView2Controller> m_ctrl;
    ComPtr<ICoreWebView2> m_view;

    QTimer* m_poll = nullptr;
    QString m_currentText;
    QString m_status;
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
    m_pluginDir = pluginDir;

    setAttribute(Qt::WA_NativeWindow, true);
    m_hwnd = reinterpret_cast<HWND>(winId());
    if (m_hwnd == nullptr) {
        showError(QStringLiteral("无法创建原生窗口 (winId 失败)"));
        return false;
    }

    // WebView2 需要 STA。Qt 不会自动初始化 COM，这里自己来。
    m_coInitResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comInitialized = (m_coInitResult == S_OK);   // S_FALSE 表示本来就有，不能再 uninit
    if (FAILED(m_coInitResult))
        qWarning() << "[ndd-mdview] CoInitializeEx failed, hr=0x" << (quint32)m_coInitResult;

    // WebView2 的 loader 是**静态链接**进来的（见 CMakeLists 里的 /WHOLEARCHIVE），
    // 所以直接调用即可，不需要 LoadLibrary 去动态找 WebView2Loader.dll。
    QPointer<MdViewPanel> self(this);
    EnvCompletedHandler* envHandler = new EnvCompletedHandler(
        [self](HRESULT code, ICoreWebView2Environment* env) {
            if (!self)
                return;
            if (FAILED(code) || env == nullptr) {
                self->showError(QStringLiteral("创建 WebView2 环境失败 (hr=0x%1)")
                                    .arg((quint32)code, 8, 16, QLatin1Char('0')));
                return;
            }
            self->m_env = env;
            self->createController();
        });

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr, envHandler);
    if (FAILED(hr)) {
        // 退回到老接口（少一个参数，某些版本 loader 只导这个）
        HRESULT hr2 = CreateCoreWebView2Environment(envHandler);
        if (SUCCEEDED(hr2))
            hr = hr2;
    }
    envHandler->Release();     // 交出我们这份引用，COM 那份归它自己
    if (FAILED(hr)) {
        showError(QStringLiteral("创建 WebView2 环境失败 (hr=0x%1)")
                      .arg((quint32)hr, 8, 16, QLatin1Char('0')));
        return false;
    }
    qWarning() << "[ndd-mdview] CreateCoreWebView2EnvironmentWithOptions hr=0x"
               << (quint32)hr;
    setStatus(QStringLiteral("env ok"));

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
    CtrlCompletedHandler* ctrlHandler = new CtrlCompletedHandler(
        [self](HRESULT code, ICoreWebView2Controller* ctrl) {
            if (!self)
                return;
            if (FAILED(code) || ctrl == nullptr) {
                self->showError(QStringLiteral("创建 WebView2 控制器失败 (hr=0x%1)")
                                    .arg((quint32)code, 8, 16, QLatin1Char('0')));
                return;
            }
            self->m_ctrl = ctrl;

            ComPtr<ICoreWebView2> view;
            if (FAILED(self->m_ctrl->get_CoreWebView2(&view)) || view == nullptr) {
                self->showError(QStringLiteral("取得 WebView2 实例失败"));
                return;
            }
            self->m_view = view;
            self->setStatus(QStringLiteral("ctrl ok"));
            self->syncBounds();
            self->navigateToApp();
            self->logChildren();
        });
    m_env->CreateCoreWebView2Controller(m_hwnd, ctrlHandler);
    ctrlHandler->Release();
}

void MdViewPanel::navigateToApp()
{
    if (!m_view)
        return;

    // markdown-it 脚本：优先用编译进 DLL 的 Qt 资源；
    // 资源缺失时回退到插件目录下的同名文件。
    QString mdJs;
    {
        QFile f(QString::fromUtf8(kMarkdownItResource));
        if (f.open(QIODevice::ReadOnly)) {
            mdJs = QString::fromUtf8(f.readAll());
        } else {
            const QStringList candidates = {
                QDir(m_pluginDir).filePath(QStringLiteral("markdown-it.min.js")),
                QDir(m_pluginDir).filePath(QStringLiteral("mdview/markdown-it.min.js")),
            };
            for (const QString& p : candidates) {
                QFile g(p);
                if (g.open(QIODevice::ReadOnly)) {
                    mdJs = QString::fromUtf8(g.readAll());
                    break;
                }
            }
        }
    }
    if (mdJs.isEmpty()) {
        showError(QStringLiteral("找不到 markdown-it 脚本："
                                 "既没有编译进 DLL，也没有放在插件目录下。"));
        return;
    }

    const QString html = buildHtml(mdJs);

    QPointer<MdViewPanel> self(this);
    EventRegistrationToken token;
    NavCompletedHandler* navHandler = new NavCompletedHandler([self]() {
        if (!self)
            return;
        self->m_pageReady = true;
        self->setStatus(QStringLiteral("page ok"));
        self->m_lastPayload.clear();
        self->pushCurrentText();
    });
    HRESULT hr = m_view->add_NavigationCompleted(navHandler, &token);
    navHandler->Release();
    (void)hr;

    m_view->NavigateToString(reinterpret_cast<LPCWSTR>(html.utf16()));

    // 乐观地先推一次；等 NavigationCompleted 再强制推一次（页面刚加载完时
    // 之前的 ExecuteScript 会丢失）
    m_pageReady = true;
    m_lastPayload.clear();
    pushCurrentText();
}

void MdViewPanel::logChildren() const
{
    if (m_hwnd == nullptr)
        return;
    wchar_t cls[256];
    cls[0] = L'\0';
    GetClassNameW(m_hwnd, cls, 256);
    RECT r;
    GetClientRect(m_hwnd, &r);
    qWarning() << "[ndd-mdview] panel hwnd class=" << QString::fromWCharArray(cls)
               << " client=" << (r.right - r.left) << "x" << (r.bottom - r.top);

    // 列一下子窗口，看看 WebView2 的子窗口在不在、大小对不对
    struct Ctx { QStringList out; };
    Ctx ctx;
    EnumChildWindows(m_hwnd, [](HWND h, LPARAM lp) -> BOOL {
        Ctx* c = reinterpret_cast<Ctx*>(lp);
        wchar_t c2[256];
        c2[0] = L'\0';
        GetClassNameW(h, c2, 256);
        RECT rr;
        GetClientRect(h, &rr);
        c->out << QStringLiteral("%1 %2x%3").arg(QString::fromWCharArray(c2))
                      .arg(rr.right - rr.left).arg(rr.bottom - rr.top);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    qWarning() << "[ndd-mdview] panel children:" << ctx.out.join(' | ');
}

void MdViewPanel::pushCurrentText()
{
    if (!m_view || !m_pageReady)
        return;

    const QByteArray payload = m_currentText.toUtf8().toBase64();
    if (payload == m_lastPayload)
        return;
    m_lastPayload = payload;
    setStatus(QStringLiteral("实时 | %1 字符").arg(m_currentText.size()));
    qWarning() << "[ndd-mdview] push" << payload.size() << "bytes";

    const QString js = QStringLiteral("window.mdUpdate('")
        + QString::fromLatin1(payload) + QStringLiteral("')");

    // ExecuteScript 必须带完成回调（这个 SDK 版本的签名是 2 参数）
    ExecScriptHandler* execHandler = new ExecScriptHandler(
        [](HRESULT code, LPCWSTR) {
            if (FAILED(code))
                qWarning() << "[ndd-mdview] ExecuteScript failed, hr=0x"
                           << (quint32)code;
        });
    m_view->ExecuteScript(reinterpret_cast<LPCWSTR>(js.utf16()), execHandler);
    execHandler->Release();
}

void MdViewPanel::pollNow()
{
    if (!m_host)
        return;

    // 顺手再对一次 bounds：停靠/浮动时 Qt 可能不触发 resizeEvent
    if (m_ctrl)
        syncBounds();

    // 宿主的 qscintilla 模块可能比插件晚加载，这里兜底重试
    if (!m_textFn) {
        m_textFn = ResolveSciTextFn();
        if (!m_textFn)
            return;
    }

    // 用 Qt 对象树定位当前编辑区（不走宿主的 std::function 仿函数）
    QWidget* ed = FindCurrentEditor(m_host.data());
    if (ed == nullptr)
        return;
    if (ed != m_lastEditor) {
        m_lastEditor = ed;
        qWarning() << "[ndd-mdview] 当前编辑区 =" << (void*)ed
                   << " class=" << ed->metaObject()->className()
                   << " size=" << ed->width() << "x" << ed->height();
    }

    QString text;
    if (!SafeSciText(m_textFn, reinterpret_cast<QsciScintilla*>(ed), &text)) {
        qWarning() << "[ndd-mdview] 调用 QsciScintilla::text() 发生访问违例，"
                      "编辑器指针=" << (void*)ed << "，本轮跳过";
        return;
    }
    if (text == m_currentText)
        return;

    m_currentText = text;
    pushCurrentText();
}

void MdViewPanel::showError(const QString& msg)
{
    qWarning() << "[ndd-mdview]" << msg;

    // WebView2 都没起来的时候没法往面板里画东西，
    // 至少把停靠窗格的标题和 tooltip 改掉，让人一眼看出是哪儿坏了。
    if (m_dock) {
        m_dock->setWindowTitle(QStringLiteral("Markdown 预览启动失败"));
        m_dock->setToolTip(msg);
    }

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
    std::function<QsciScintilla*(QWidget*)> getCurEdit;
    SciTextFn textFn = nullptr;

    void ensure()
    {
        if (dock || panel)
            return;

        // 取文入口在这里才解析 —— 插件被扫描时 qmyedit_qt5.dll 未必已经加载，
        // 而等到用户真正点菜单时，宿主早就把编辑区建起来了。
        if (!textFn)
            textFn = ResolveSciTextFn();
        if (textFn == nullptr) {
            qWarning() << "[ndd-mdview] 找不到宿主导出的 QsciScintilla::text()"
                          "（符号 " << kSciTextSymbol << "），预览将没有内容";
        } else {
            qWarning() << "[ndd-mdview] 已定位宿主导出的 QsciScintilla::text()";
        }

        QMainWindow* mw = host ? qobject_cast<QMainWindow*>(host.data()) : nullptr;
        if (mw) {
            dock = new QDockWidget(QStringLiteral("Markdown 实时预览"), mw);
            dock->setObjectName(QStringLiteral("ndd_mdview_dock"));
            panel = new MdViewPanel(dock);
            panel->setHostWidget(host);
            panel->setDockWidget(dock);
            panel->setTextSource(getCurEdit, textFn);
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
            panel->setTextSource(getCurEdit, textFn);
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
    Q_UNUSED(pluginCallBack);

    // m_menuType==0 的正常路径下 pProcData 有效（含 m_pAction）。
    // 另一条路径（宿主 onPlugWork）传的是 nullptr，那种情况没法挂动作，直接退出。
    if (pProcData == nullptr || pProcData->m_pAction == nullptr)
        return -1;

    // 取文入口：宿主导出的 QsciScintilla::text()（延迟解析）
    SciTextFn textFn = ResolveSciTextFn();
    if (textFn == nullptr) {
        qWarning() << "[ndd-mdview] 启动时未找到 QsciScintilla::text()，"
                      "将在使用时重试";
    }

    MdController* ctl = new MdController(pNotepad);
    ctl->host = pNotepad;
    ctl->pluginDir = QFileInfo(strFileName).absolutePath();
    ctl->getCurEdit = getCurEdit;
    ctl->textFn = textFn;
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
