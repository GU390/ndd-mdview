// ============================================================================
//  ndd-mdview.cpp
//  Notepad-- 插件：内嵌 Microsoft Edge WebView2 的 Markdown 实时预览
//
//  设计要点
//  --------
//  1) 宿主 ABI（src/pluginGl.h，与 Notepad-- v3.8.3 一致）：
//       NDD_PROC_IDENTIFY : 扫描 plugin\*.dll 时调用，填名字/说明/版本
//       NDD_PROC_MAIN     : 宿主建好二级菜单后调用，插件往里塞自己的菜单项
//     m_menuType==1 时宿主建二级菜单并把句柄塞进 pProcData->m_rootMenu，
//     菜单项由插件自己加（见 ccnotepad.cpp 的 onPlugFound）。
//
//  2) 取编辑区文本 **不链接 qscint**：
//     宿主把 qscintilla 静态链进 qmyedit_qt5.dll，符号全部导出（实测 2403 项），
//     头文件不随插件分发，类布局跨版本还会变。
//     所以只声明一个同名同签名的最小类，用 GetProcAddress 拿真实函数地址。
//     ⚠️ 调用约定是**成员函数**那套：RCX=this、RDX=隐藏返回缓冲区
//     （自由函数才是 RCX=返回槽、RDX=第一个实参）。搞反了就是崩宿主。
//     详见下面「取文工具层」的注释。
//
//  3) 文本直接用 PostWebMessageAsString 推给网页（不是 base64+ExecuteScript）：
//     省掉 UTF-8 转换、base64 编码、JS 表达式拼装与再解析，每帧少三次大拷贝。
//
//  4) 面板用 QDockWidget 挂在主窗口右侧；宿主主窗口是 QMainWindow
//     （CCNotePad : public QMainWindow），qobject_cast 判定。
//     退化路径：不是 QMainWindow 时退化成独立顶层窗口。
//
//  5) 更新策略：默认**事件驱动** —— 连编辑区的 textChanged() 信号，
//     120ms 合并成一次推送；空闲时完全不轮询，CPU 归零。
//     也可切成固定间隔轮询（用户可调），适合超大文件。
//
//  6) 公式渲染：KaTeX 0.16.11 + markdown-it-katex。
//     KaTeX 的静态资源编译进 DLL，首次使用时解到 %TEMP%，
//     再用 WebView2 的虚拟主机映射（https://mdview.local/）暴露给网页，
//     字体按需加载，不会全进内存。
//
//  7) 本文件含 Q_OBJECT（MdViewPanel / MdController），moc 由 CMake AUTOMOC 生成，
//     见文件末尾的 #include "ndd-mdview.moc"。
// ============================================================================

#include <QtGlobal>
#include <QWidget>
#include <QObject>
#include <QApplication>
#include <QAction>
#include <QActionGroup>
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
#include <QMessageBox>
#include <QPointer>
#include <QSettings>
#include <QShowEvent>
#include <QHideEvent>
#include <QResizeEvent>
#include <QDebug>

#include <functional>
#include <vector>
#include <cstring>

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

// ===========================================================================
//  设置（QSettings 持久化到 %APPDATA%\Notepad--\ndd-mdview.ini）
// ===========================================================================

// 自动更新模式：0=实时（事件驱动），其余是固定轮询间隔
static const int kRefreshIntervals[] = { 0, 500, 1000, 2000, 5000 };
static const int kRefreshModeCount = 5;
static const char* kRefreshLabels[] = {
    "实时（事件驱动）", "500 毫秒", "1 秒", "2 秒", "5 秒"
};

struct MdSettings
{
    int  refreshMode   = 0;    // 见 kRefreshIntervals
    bool katex        = true;  // 公式渲染
    bool releaseOnHide = true; // 隐藏面板时释放 WebView2 内存

    static MdSettings load()
    {
        MdSettings s;
        QSettings st(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("Notepad--"), QStringLiteral("ndd-mdview"));
        s.refreshMode = st.value(QStringLiteral("refreshMode"), 0).toInt();
        if (s.refreshMode < 0 || s.refreshMode >= kRefreshModeCount)
            s.refreshMode = 0;
        s.katex = st.value(QStringLiteral("katex"), true).toBool();
        s.releaseOnHide = st.value(QStringLiteral("releaseOnHide"), true).toBool();
        return s;
    }

    void save() const
    {
        QSettings st(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("Notepad--"), QStringLiteral("ndd-mdview"));
        st.setValue(QStringLiteral("refreshMode"), refreshMode);
        st.setValue(QStringLiteral("katex"), katex);
        st.setValue(QStringLiteral("releaseOnHide"), releaseOnHide);
        st.sync();
    }
};

// ===========================================================================
//  取文工具层
// ===========================================================================

// ABI 签名要求 std::function<QsciScintilla*(QWidget*)>，这里的名字必须
// 与宿主一致（模板实参只用到指针类型，名字不影响 ABI）。
class QsciScintilla
{
public:
};

// this=RCX，返回 bool（AL）
typedef bool (*SciBoolFn)(QsciScintilla* self);
// this=RCX，返回 int（EAX）
typedef int (*SciIntFn)(QsciScintilla* self);
// this=RCX，参数 EDX/R8D/R9D，返回 long（EAX）
typedef long (*SciSendFn)(QsciScintilla* self, unsigned int msg,
                          unsigned long wParam, long lParam);
// ⚠️ 成员函数按值返回类类型时：**RCX=this，RDX=隐藏返回缓冲区**
//    （自由函数才是 RCX=返回槽、RDX=第一个实参）。
//    用 capstone dump qmyedit_qt5.dll 的 text() 机器码核对过：
//      mov r14, rdx        ; 取回入口 RDX（返回槽）
//      mov rbp, rcx        ; 存入口 RCX（this）
//      call SendScintilla  ; RCX 未改 —— 它内部 mov rcx,[rcx+30]，
//                           ; 所以 RCX 必须是 this
//      mov rax, r14        ; 返回槽地址放 RAX
//    一开始按自由函数的规则写（RCX=槽、RDX=this）→ 被调方把返回槽当 this
//    解引用 [rcx+0x30] → 越界访问违例 → 被 SEH 吞掉 → 静默走了读文件兜底。
typedef void (*SciTextFn)(void* self, void* retSlot);

// 宿主 qscintilla 那个 DLL 里的修饰名
static const char* kIsReadOnlySymbol = "?isReadOnly@QsciScintilla@@QEBA_NXZ";
static const char* kIsModifiedSymbol = "?isModified@QsciScintilla@@QEBA_NXZ";
static const char* kCurLineSymbol = "?getCurrentLineNumber@QsciScintilla@@QEBAHXZ";
static const char* kSendScintillaSymbol = "?SendScintilla@QsciScintillaBase@@QEBAJIKJ@Z";
static const char* kTextSymbol = "?text@QsciScintilla@@QEBA?AVQString@@XZ";

// 在已加载模块里找导出该符号的 DLL，返回函数地址
static void* ResolveExport(const char* symbol)
{
    static const char* kCandidates[] = {
        "qmyedit_qt5.dll",
        "qmyedit_qt6.dll",
        "qmyedit.dll",
        "qscintilla.dll",
    };
    for (size_t i = 0; i < sizeof(kCandidates) / sizeof(kCandidates[0]); ++i) {
        HMODULE h = GetModuleHandleA(kCandidates[i]);
        if (h) {
            FARPROC p = GetProcAddress(h, symbol);
            if (p)
                return reinterpret_cast<void*>(p);
        }
    }

    // 兜底：遍历本进程已加载的所有模块
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
    void* found = nullptr;
    if (enumMods) {
        HMODULE mods[1024];
        DWORD needed = 0;
        if (enumMods(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
            DWORD count = needed / sizeof(HMODULE);
            for (DWORD i = 0; i < count && found == nullptr; ++i) {
                FARPROC p = GetProcAddress(mods[i], symbol);
                if (p)
                    found = reinterpret_cast<void*>(p);
            }
        }
    }
    if (psapi)
        FreeLibrary(psapi);
    return found;
}

// ---------------------------------------------------------------------------
//  定位当前编辑区
//
//  两个来源互相印证：
//   1) 宿主 ABI 里的 getCurEdit 仿函数（返回 QsciScintilla*）
//   2) Qt 对象树：findChildren<QWidget*> + metaObject()->className()
//      —— 优先选祖先里有 QTabWidget 的那个（宿主主编辑区是 ui.editTabWidget 的页，
//      别处也可能藏着 ScintillaEditView）
//
//  关键前提：ScintillaEditView : QsciScintilla : QsciScintillaBase
//  : QAbstractScrollArea : QFrame : QWidget : QObject —— 全是单继承，
//  所以 **所有基类子对象的偏移都是 0**：QWidget* == QsciScintilla*。
// ---------------------------------------------------------------------------

// 往上找第一个 QTabWidget 祖先（找不到返回 nullptr）
static QWidget* FindTabAncestor(QWidget* w)
{
    for (QWidget* p = w ? w->parentWidget() : nullptr; p; p = p->parentWidget()) {
        const QMetaObject* mo = p->metaObject();
        if (mo && qstrcmp(mo->className(), "QTabWidget") == 0)
            return p;
    }
    return nullptr;
}

static QWidget* FindCurrentEditor(QWidget* host)
{
    if (host == nullptr)
        return nullptr;

    QWidget* focus = QApplication::focusWidget();
    QWidget* best = nullptr;
    long bestArea = -1;
    QWidget* bestTab = nullptr;
    long bestTabArea = -1;

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

        const QRect r = w->rect();
        const long area = (long)r.width() * (long)r.height();

        if (FindTabAncestor(w)) {
            if (area > bestTabArea) {
                bestTabArea = area;
                bestTab = w;
            }
        }
        if (w == focus)
            return w;                       // 有焦点的优先
        if (area > bestArea) {
            bestArea = area;
            best = w;
        }
    }
    return bestTab ? bestTab : best;
}

// ---------------------------------------------------------------------------
//  取文：SCI 消息 + 低地址缓冲区
// ---------------------------------------------------------------------------

// Scintilla 消息号（公开 API，跨版本稳定）
#define SCI_GETLENGTH      2006
#define SCI_GETTEXT        2182
#define SCI_GETCODEPAGE    2137

// 在 4GB 以下申请一块缓冲区。
// SendScintilla 的 lParam 修饰名里是 32 位（J/K），指针有可能被截断，
// 所以缓冲区必须落在低地址 —— 指定起始地址让系统在那里分配。
static char* AllocLowBuffer(SIZE_T size)
{
    void* p = VirtualAlloc((void*)(uintptr_t)0x00010000, size,
                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return static_cast<char*>(p);
}

// 按 Scintilla 文档编码把原始字节解成 QString
static QString DecodeDocBytes(const char* buf, int len, int codePage)
{
    if (buf == nullptr || len <= 0)
        return QString();

    if (codePage == 0 || codePage == 65001) {          // 0 / CP_UTF8
        return QString::fromUtf8(buf, len);
    }
    if (codePage == 1200 || codePage == 1201) {         // UTF-16 LE / BE
        return QString::fromUtf16(reinterpret_cast<const ushort*>(buf), len / 2);
    }
    int wlen = MultiByteToWideChar(codePage, 0, buf, len, nullptr, 0);
    if (wlen <= 0)
        return QString::fromUtf8(buf, len);
    std::vector<wchar_t> wide((size_t)wlen + 1, 0);
    MultiByteToWideChar(codePage, 0, buf, len, wide.data(), wlen);
    return QString::fromWCharArray(wide.data(), wlen);
}

// 探针 + 取长度 + 取编码。__try 里不放任何需要回卷的 C++ 对象
// （否则 MSVC 报 C2712），所以只处理指针和 int。
static bool SafeSciProbe(SciBoolFn isRO, SciSendFn send, QsciScintilla* ed,
                         int* length, int* codePage, bool* readOnly)
{
    __try {
        *readOnly = isRO(ed);
        *length = (int)send(ed, SCI_GETLENGTH, 0, 0);
        *codePage = (int)send(ed, SCI_GETCODEPAGE, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 取全文（原始字节进 buf）
static bool SafeSciText(SciSendFn send, QsciScintilla* ed, char* buf, int cap, int length)
{
    __try {
        if (length > 0 && length < cap)
            send(ed, SCI_GETTEXT, (unsigned long)length + 1, (long)(intptr_t)buf);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 探针：bool isReadOnly() const（this=RCX，返回 AL）
static bool SafeSciBool(SciBoolFn fn, QsciScintilla* ed, bool* out)
{
    __try {
        *out = fn(ed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 探针：int getCurrentLineNumber() const（this=RCX，返回 EAX）
static bool SafeSciInt(SciIntFn fn, QsciScintilla* ed, int* out)
{
    __try {
        *out = fn(ed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 调 QsciScintilla::text() 取全文。
//
// 约定：RCX = this，RDX = 隐藏返回缓冲区（见 SciTextFn 的注释）。
// 返回缓冲区用**裸缓冲**而不是 QString 局部变量：
// 万一调用过程中崩了，槽里可能是个半构造的 QString，
// 交给 Qt 的析构函数去释放就会踩坏堆（实测会变成 BEX64 栈/缓冲区溢出）。
// 用裸缓冲就没有这个风险 —— 崩了直接丢弃这块内存。
//
// 拆成两个函数：CallTextRaw 里没有任何 C++ 对象（否则 MSVC 报 C2712），
// QString 的拷贝放在外面做。
static bool CallTextRaw(SciTextFn fn, void* self, void* slot)
{
    __try {
        fn(self, slot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 槽里现在是一个构造好的 QString：拷出来，并释放槽里那一份
static void TakeTextFromSlot(void* slot, QString* out)
{
    QString* q = reinterpret_cast<QString*>(slot);
    *out = *q;              // 拷出来
    q->~QString();          // 释放槽里那个
}

// ===========================================================================
//  静态资源：KaTeX 解到 %TEMP%
//
//  KaTeX 的 js/css/字体编译进 DLL（Qt 资源），首次使用时解到临时目录，
//  再用 WebView2 的虚拟主机映射暴露给网页。
//  好处：字体按需加载（不会 20 个字体全进内存）、页面本体很小。
// ===========================================================================
static const char* kMarkdownItResource = ":/mdview/markdown-it.min.js";
static const char* kMathPluginResource = ":/mdview/mdview-math.js";
static const char* kKatexJsResource    = ":/mdview/katex.min.js";
static const char* kKatexCssResource   = ":/mdview/katex.min.css";
static const char* kKatexFontResource  = ":/mdview/fonts/";
static const char* kKatexMarker        = "assets.ok";

// 临时目录：%TEMP%\ndd-mdview\katex-0.16.11
static QString KatexAssetDir()
{
    return QDir(QDir::tempPath())
        .filePath(QStringLiteral("ndd-mdview") + QDir::separator()
                  + QStringLiteral("katex-0.16.11"));
}

// 把一个资源写成文件
static bool WriteResourceToFile(const QString& resPath, const QString& filePath)
{
    QFile f(resPath);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QFile out(filePath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    out.write(f.readAll());
    out.close();
    return true;
}

// 解包 KaTeX 资源（幂等：靠标记文件判断是否已解过）
static bool ExtractKatexAssets(QString* err)
{
    const QString dir = KatexAssetDir();
    QDir d(dir);
    if (!d.exists() && !d.mkpath(QStringLiteral("."))) {
        *err = QStringLiteral("无法创建临时目录：%1").arg(dir);
        return false;
    }

    const QString marker = d.filePath(QStringLiteral("assets.ok"));
    if (QFile::exists(marker))
        return true;

    // 字体
    const QString fontDir = d.filePath(QStringLiteral("fonts"));
    QDir().mkpath(fontDir);
    QDir res(QStringLiteral(":/mdview/fonts"));
    const QStringList fonts = res.entryList(QDir::Files);
    for (int i = 0; i < fonts.size(); ++i) {
        if (!WriteResourceToFile(QStringLiteral(":/mdview/fonts/") + fonts.at(i),
                                 d.filePath(QStringLiteral("fonts/") + fonts.at(i)))) {
            *err = QStringLiteral("解包字体失败：%1").arg(fonts.at(i));
            return false;
        }
    }

    if (!WriteResourceToFile(QString::fromLatin1(kKatexJsResource), d.filePath(QStringLiteral("katex.min.js")))
        || !WriteResourceToFile(QString::fromLatin1(kKatexCssResource), d.filePath(QStringLiteral("katex.min.css"))) ) {
        *err = QStringLiteral("解包 KaTeX 脚本失败");
        return false;
    }

    QFile m(marker);
    if (m.open(QIODevice::WriteOnly)) {
        m.write("ok\n");
        m.close();
    }
    return true;
}

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

// ===========================================================================
//  预览面板
// ===========================================================================
class MdViewPanel : public QWidget
{
    Q_OBJECT
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
        if (m_coalesce) {
            m_coalesce->stop();
        }
        if (m_buf) {
            VirtualFree(m_buf, 0, MEM_RELEASE);
            m_buf = nullptr;
            m_bufCap = 0;
        }
        if (m_comInitialized) {
            CoUninitialize();
        }
    }

    void setHostWidget(QWidget* w) { m_host = w; }
    void setDockWidget(QDockWidget* w) { m_dock = w; }

    // 取文来源：宿主给的编辑区仿函数 + qscintilla 导出的入口
    void setTextSource(std::function<QsciScintilla*(QWidget*)> getCurEdit,
                       SciBoolFn isRO, SciSendFn send, SciTextFn text)
    {
        m_getCurEdit = std::move(getCurEdit);
        m_isROFn = isRO;
        m_sendFn = send;
        m_textFn = text;
    }

    void setSettings(const MdSettings& s) { m_settings = s; }
    MdSettings settings() const { return m_settings; }

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

    // 立即刷新一次（菜单用）
    void refreshNow();

    // 设置变了：重新装载页面
    void reloadPage();

    // 隐藏时按设置释放 WebView2 内存
    void releaseWebView();

public slots:
    // 编辑区内容变了（事件驱动路径）
    void onEditorTextChanged();
    // 标签页切换了
    void onTabCurrentChanged(int index);

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
        refreshNow();

        // 布局可能还没稳定，过一会儿再对一次 bounds、并补一次推送
        QTimer::singleShot(0, this, [this]() { syncBounds(); });
        QTimer::singleShot(400, this, [this]() {
            syncBounds();
            m_lastText.clear();
            pushCurrentText();
        });
    }

    void hideEvent(QHideEvent* e) override
    {
        if (m_poll)
            m_poll->stop();
        if (m_coalesce)
            m_coalesce->stop();
        QWidget::hideEvent(e);
        if (m_settings.releaseOnHide)
            releaseWebView();
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
    bool readCurrentText(QString& out);
    void showError(const QString& msg);
    void logChildren() const;
    void applyRefreshMode();
    void connectEditor(QWidget* ed);

    QPointer<QWidget> m_host;
    QPointer<QDockWidget> m_dock;
    std::function<QsciScintilla*(QWidget*)> m_getCurEdit;
    SciBoolFn m_isROFn = nullptr;
    SciSendFn m_sendFn = nullptr;
    SciTextFn m_textFn = nullptr;
    char* m_buf = nullptr;
    int m_bufCap = 0;
    QWidget* m_lastEditor = nullptr;
    QWidget* m_connEditor = nullptr;
    QWidget* m_connTab = nullptr;
    bool m_probed = false;
    bool m_warnedFallback = false;
    MdSettings m_settings;
    HWND m_hwnd = nullptr;
    QString m_pluginDir;
    HRESULT m_coInitResult = S_OK;
    bool m_comInitialized = false;

    ComPtr<ICoreWebView2Environment> m_env;
    ComPtr<ICoreWebView2Controller> m_ctrl;
    ComPtr<ICoreWebView2> m_view;

    QTimer* m_poll = nullptr;       // 轮询模式下的定时器
    QTimer* m_coalesce = nullptr;  // 事件模式下的合并定时器
    QString m_currentText;
    QString m_lastText;
    QString m_status;
    bool m_pageReady = false;
    bool m_usePostMessage = false;   // 页面是否支持原生消息通道
};

// --- 面板实现 ---------------------------------------------------------------

void MdViewPanel::applyRefreshMode()
{
    const int interval = kRefreshIntervals[m_settings.refreshMode];
    if (!m_poll)
        return;
    if (interval <= 0) {
        // 实时模式：靠 textChanged 信号，不轮询
        m_poll->stop();
    } else {
        m_poll->setInterval(interval);
        if (isVisible())
            m_poll->start();
    }
}

void MdViewPanel::connectEditor(QWidget* ed)
{
    if (m_connEditor == ed)
        return;

    if (m_connEditor)
        m_connEditor->disconnect(this);
    m_connEditor = ed;

    if (ed) {
        // 字符串式连接：不需要 QsciScintilla 的类定义。
        // QsciScintilla 有 textChanged() 信号（宿主自己就用它连 Markdown 预览）。
        QObject::connect(ed, SIGNAL(textChanged()), this, SLOT(onEditorTextChanged()));
    }

    // 标签页切换时换编辑区
    QWidget* tab = FindTabAncestor(ed);
    if (m_connTab != tab) {
        if (m_connTab)
            m_connTab->disconnect(this);
        m_connTab = tab;
        if (tab)
            QObject::connect(tab, SIGNAL(currentChanged(int)), this, SLOT(onTabCurrentChanged(int)));
    }
}

void MdViewPanel::onEditorTextChanged()
{
    // 合并 120ms 内的连续变化，避免打字时每个键都重渲染一次
    if (m_coalesce)
        m_coalesce->start(120);
}

void MdViewPanel::onTabCurrentChanged(int)
{
    m_lastEditor = nullptr;   // 强制重新定位编辑区
    m_lastText.clear();
    refreshNow();
}

void MdViewPanel::refreshNow()
{
    pollNow();
}

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
        "#md pre code{background:none;padding:0;font-size:1em;line-height:1.5;}"
        "#md blockquote{margin:.8em 0;padding:.2em 1em;color:#57606a;"
        "border-left:4px solid #d0d7de;background:#f8f9fb;}"
        "#md table{border-collapse:collapse;margin:1em 0;display:block;overflow:auto;}"
        "#md th,#md td{border:1px solid #d0d7de;padding:6px 13px;}"
        "#md th{background:#f6f8fa;font-weight:600;}"
        "#md ul,#md ol{padding-left:2em;margin:.6em 0;}"
        "#md li{margin:.2em 0;}"
        "#md img{max-width:100%;}"
        "#md hr{border:none;border-top:1px solid #d8dee4;margin:1.6em 0;}"
        /* 公式：KaTeX 排版需要，允许横向滚动 */
        "#md .katex-display{margin:1em 0;overflow-x:auto;overflow-y:hidden;}"
        "#md .katex{font-size:1.05em;}"
        /* 公式里夹中文：KaTeX 自带字体没有汉字，靠浏览器逐字回退 */
        "#md .katex .mord.text,#md .katex .text{font-family:inherit;}"
        /* 公式语法错误时退回显示原始 LaTeX */
        "#md code.md-math-error{color:#b42318;background:#fff5f5;"
        "border:1px solid #f3c0c0;white-space:pre-wrap;}";

    // JS 侧：window.mdRender(text) -> 渲染
    static const char* kApi =
        "(function(){"
        "  var md = window.markdownit({html:false,linkify:true,breaks:false,"
        "typographer:false,xhtmlOut:false});"
        "  if (window.katex && window.mdviewMath) {"
        "    try { md.use(window.mdviewMath, {throwOnError:false,strict:false,trust:false}); }"
        "    catch(e) { console.log('math plugin failed', e); }"
        "  }"
        "  window.mdRender = function(text){"
        "    try{"
        "      document.getElementById('md').innerHTML = md.render(text);"
        "    }catch(e){"
        "      document.getElementById('md').textContent = 'Render error: '+e;"
        "    }"
        "  };"
        "  window.mdUpdate = function(b64){"
        "    try{"
        "      var bin = atob(b64);"
        "      var bytes = new Uint8Array(bin.length);"
        "      for(var i=0;i<bin.length;i++){bytes[i]=bin.charCodeAt(i);}"
        "      var text = new TextDecoder('utf-8').decode(bytes);"
        "      window.mdRender(text);"
        "    }catch(e){"
        "      document.getElementById('md').textContent = 'Render error: '+e;"
        "    }"
        "  };"
        /* 能不能用原生消息通道（宿主 PostWebMessageAsString）？
           页面加载完宿主会问一句 window.mdvSupportsPost，
           答 1 就走原生通道（省掉 base64 编码和三次拷贝），
           答 0 宿主就退回 base64 + ExecuteScript。 */
        "  window.mdvSupportsPost = false;"
        "  try{"
        "    if (window.chrome && window.chrome.webview &&"
        "        window.chrome.webview.addEventListener) {"
        "      window.chrome.webview.addEventListener('message', function(e){"
        "        window.mdRender(e.data);"
        "      });"
        "      window.mdvSupportsPost = true;"
        "    }"
        "  }catch(e){ window.mdvSupportsPost = false; }"
        "})();";

    // KaTeX 资源：js/css 从虚拟主机 https://mdview.local/ 取，
    // 公式插件（我们自己的）直接内联。
    QString katexHead;
    QString katexTail;
    if (m_settings.katex) {
        katexHead = QStringLiteral(
            "<link rel=\"stylesheet\" href=\"https://mdview.local/katex.min.css\">");
        katexTail = QStringLiteral(
            "<script src=\"https://mdview.local/katex.min.js\"></script>");
        QFile mf(QString::fromLatin1(kMathPluginResource));
        if (mf.open(QIODevice::ReadOnly)) {
            katexTail += QStringLiteral("<script>");
            katexTail += QString::fromUtf8(mf.readAll());
            katexTail += QStringLiteral("</script>");
        } else {
            qWarning() << "[ndd-mdview] 找不到公式插件资源，公式渲染不可用";
        }
    }

    QString html;
    html.reserve(mdJs.size() + 8192);
    html += QStringLiteral("<!DOCTYPE html><html><head><meta charset=\"utf-8\">");
    html += QStringLiteral("<title>Markdown Preview</title>");
    html += katexHead;
    html += QStringLiteral("<style>");
    html += QString::fromUtf8(kCss);
    html += QStringLiteral("</style></head><body>");
    html += QStringLiteral("<div id=\"md\"></div>");
    html += QStringLiteral("<script>");
    html += mdJs;
    html += QStringLiteral("</script>");
    html += katexTail;
    html += QStringLiteral("<script>");
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

    // 合并定时器：事件模式下把连续变化压成一次推送
    m_coalesce = new QTimer(this);
    m_coalesce->setSingleShot(true);
    QObject::connect(m_coalesce, &QTimer::timeout, this, [this]() { pollNow(); });

    // 轮询定时器：只在用户选了固定间隔时启用
    m_poll = new QTimer(this);
    QObject::connect(m_poll, &QTimer::timeout, this, [this]() { pollNow(); });
    applyRefreshMode();

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

    // KaTeX 资源：解包到 %TEMP%，再用虚拟主机映射暴露给网页
    if (m_settings.katex) {
        QString err;
        if (!ExtractKatexAssets(&err)) {
            qWarning() << "[ndd-mdview]" << err << "（公式渲染不可用）";
        } else {
            ComPtr<ICoreWebView2_3> view3;
            if (SUCCEEDED(m_view.As(&view3)) && view3) {
                HRESULT hr = view3->SetVirtualHostNameToFolderMapping(
                    L"mdview.local",
                    reinterpret_cast<LPCWSTR>(KatexAssetDir().toStdWString().c_str()),
                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                if (FAILED(hr))
                    qWarning() << "[ndd-mdview] 虚拟主机映射失败, hr=0x" << (quint32)hr;
            }
        }
    }

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
        self->m_lastText.clear();

        // 问页面支不支持原生消息通道（省掉 base64 编码和几次拷贝）。
        // 页面答不上来就退回 base64 + ExecuteScript，不影响功能。
        ExecScriptHandler* probe = new ExecScriptHandler([self](HRESULT code, LPCWSTR result) {
            if (!self)
                return;
            if (SUCCEEDED(code) && result != nullptr) {
                const QString r = QString::fromWCharArray(result);
                self->m_usePostMessage = r.contains(QStringLiteral("YES"));
            }
            qWarning() << "[ndd-mdview] 推送通道 ="
                       << (self->m_usePostMessage ? QStringLiteral("PostWebMessage")
                                                   : QStringLiteral("base64+ExecuteScript"));
            self->pushCurrentText();
        });
        self->m_view->ExecuteScript(L"window.mdvSupportsPost ? 'YES' : 'NO'", probe);
        probe->Release();
    });
    HRESULT hr = m_view->add_NavigationCompleted(navHandler, &token);
    navHandler->Release();
    (void)hr;

    m_view->NavigateToString(reinterpret_cast<LPCWSTR>(html.utf16()));

    // 乐观地先推一次；等 NavigationCompleted 再强制推一次（页面刚加载完时
    // 之前的 ExecuteScript 会丢失）
    m_pageReady = true;
    m_lastText.clear();
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

    if (m_currentText == m_lastText)
        return;
    m_lastText = m_currentText;
    setStatus(QStringLiteral("实时 | %1 字符").arg(m_currentText.size()));

    if (m_usePostMessage) {
        // 原生消息通道：直接把文本字符串推给网页，
        // 省掉 UTF-8 转换、base64 编码、JS 表达式拼装与再解析。
        HRESULT hr = m_view->PostWebMessageAsString(
            reinterpret_cast<LPCWSTR>(m_currentText.utf16()));
        if (SUCCEEDED(hr))
            return;
        qWarning() << "[ndd-mdview] PostWebMessageAsString 失败, hr=0x" << (quint32)hr;
        m_usePostMessage = false;   // 退回 base64 通道
    }

    // 兜底通道：base64 + ExecuteScript
    const QByteArray payload = m_currentText.toUtf8().toBase64();
    const QString js = QStringLiteral("window.mdUpdate('")
        + QString::fromLatin1(payload) + QStringLiteral("')");
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
    if (!m_textFn && !m_sendFn) {
        m_textFn = reinterpret_cast<SciTextFn>(ResolveExport(kTextSymbol));
        m_isROFn = reinterpret_cast<SciBoolFn>(ResolveExport(kIsReadOnlySymbol));
        m_sendFn = reinterpret_cast<SciSendFn>(ResolveExport(kSendScintillaSymbol));
    }

    QString text;
    if (!readCurrentText(text))
        return;
    if (text == m_currentText)
        return;

    m_currentText = text;
    pushCurrentText();
}

bool MdViewPanel::readCurrentText(QString& out)
{
    // 候选 1：宿主给的 getCurEdit 仿函数
    QsciScintilla* fromHost = nullptr;
    if (m_getCurEdit)
        fromHost = m_getCurEdit(m_host.data());

    // 候选 2：Qt 对象树里认 className == "ScintillaEditView"
    QWidget* ed = FindCurrentEditor(m_host.data());

    // 事件驱动的信号要挂在「当前」编辑区上
    if (ed != m_lastEditor) {
        m_lastEditor = ed;
        connectEditor(ed);
    }

    if (!m_probed) {
        m_probed = true;
        qWarning() << "[ndd-mdview] 指针对照: Qt树 =" << (void*)ed
                   << " 宿主仿函数 =" << (void*)fromHost;
        if (m_isROFn) {
            bool ro = false;
            if (ed && SafeSciBool(m_isROFn, reinterpret_cast<QsciScintilla*>(ed), &ro))
                qWarning() << "[ndd-mdview] isReadOnly(Qt树指针) =" << ro;
            else
                qWarning() << "[ndd-mdview] isReadOnly(Qt树指针) -> 失败";
            if (fromHost && SafeSciBool(m_isROFn, fromHost, &ro))
                qWarning() << "[ndd-mdview] isReadOnly(宿主指针) =" << ro;
            else
                qWarning() << "[ndd-mdview] isReadOnly(宿主指针) -> 失败";
            // 再验一下读到的确实是「活的」那个编辑器
            if (ed) {
                SciBoolFn isMod = reinterpret_cast<SciBoolFn>(ResolveExport(kIsModifiedSymbol));
                SciIntFn curLine = reinterpret_cast<SciIntFn>(ResolveExport(kCurLineSymbol));
                bool mod = false;
                int line = -1;
                if (isMod && SafeSciBool(isMod, reinterpret_cast<QsciScintilla*>(ed), &mod))
                    qWarning() << "[ndd-mdview] isModified =" << mod;
                if (curLine && SafeSciInt(curLine, reinterpret_cast<QsciScintilla*>(ed), &line))
                    qWarning() << "[ndd-mdview] getCurrentLineNumber =" << line;
            }
        }
    }

    // 路 1 / 路 2：调 QsciScintilla::text()
    if (m_textFn) {
        // 64 字节：QString 本体 8 字节，留足余量，
        // 万一约定再出岔子也不会读到缓冲外面去
        alignas(8) unsigned char slot[64];
        if (fromHost) {
            memset(slot, 0, sizeof(slot));
            if (CallTextRaw(m_textFn, fromHost, slot)) {
                TakeTextFromSlot(slot, &out);
                return true;
            }
        }
        if (ed) {
            memset(slot, 0, sizeof(slot));
            if (CallTextRaw(m_textFn, ed, slot)) {
                TakeTextFromSlot(slot, &out);
                return true;
            }
        }
    }

    // 路 3：SCI 消息（SendScintilla）
    if (m_sendFn && ed) {
        int length = 0;
        int codePage = 0;
        bool readOnly = false;
        if (SafeSciProbe(m_isROFn, m_sendFn, reinterpret_cast<QsciScintilla*>(ed),
                         &length, &codePage, &readOnly) && length > 0) {
            if (m_buf == nullptr || m_bufCap < length + 2) {
                if (m_buf) {
                    VirtualFree(m_buf, 0, MEM_RELEASE);
                    m_buf = nullptr;
                    m_bufCap = 0;
                }
                const int want = qMax(length + 4096, 1 << 20);
                m_buf = AllocLowBuffer((SIZE_T)want);
                m_bufCap = m_buf ? want : 0;
            }
            if (m_buf) {
                memset(m_buf, 0, (size_t)length + 1);
                if (SafeSciText(m_sendFn, reinterpret_cast<QsciScintilla*>(ed),
                                m_buf, m_bufCap, length)) {
                    out = DecodeDocBytes(m_buf, length, codePage);
                    return true;
                }
            }
        }
    }

    // 路 4（兜底）：读磁盘上的文件。
    // 宿主把文件路径放在编辑区的 "filePath" 动态属性里（setFilePathProperty）。
    // 缺点是看不到未保存的改动，但至少预览能出内容。
    if (ed) {
        const QString path = ed->property("filePath").toString();
        if (!path.isEmpty()) {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) {
                if (!m_warnedFallback) {
                    m_warnedFallback = true;
                    qWarning() << "[ndd-mdview] 警告：读不到编辑区实时内容，"
                                  "退化到读磁盘文件（未保存的改动不会显示）";
                }
                out = QString::fromUtf8(f.readAll());
                return true;
            }
        }
    }

    return false;
}

void MdViewPanel::reloadPage()
{
    if (!m_view)
        return;
    m_pageReady = false;
    m_lastText.clear();
    navigateToApp();
}

void MdViewPanel::releaseWebView()
{
    if (!m_ctrl)
        return;
    qWarning() << "[ndd-mdview] 释放 WebView2 内存";
    m_ctrl->Close();
    m_ctrl.Reset();
    m_view.Reset();
    m_pageReady = false;
    m_lastText.clear();
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
//  控制器：管停靠窗口 + 面板 + 菜单
// ===========================================================================
class MdController : public QObject
{
    Q_OBJECT
public:
    explicit MdController(QObject* parent = nullptr) : QObject(parent) {}

    QPointer<QWidget> host;
    QPointer<QDockWidget> dock;
    MdViewPanel* panel = nullptr;
    QString pluginDir;
    QAction* actShow = nullptr;      // 「显示预览面板」勾选项，用来同步状态
    std::function<QsciScintilla*(QWidget*)> getCurEdit;
    SciBoolFn isROFn = nullptr;
    SciSendFn sendFn = nullptr;
    SciTextFn textFn = nullptr;
    MdSettings settings;

    void ensure()
    {
        if (dock || panel)
            return;

        // 取文入口在这里才解析 —— 插件被扫描时 qmyedit_qt5.dll 未必已经加载，
        // 而等到用户真正点菜单时，宿主早就把编辑区建起来了。
        if (!textFn) {
            textFn = reinterpret_cast<SciTextFn>(ResolveExport(kTextSymbol));
            isROFn = reinterpret_cast<SciBoolFn>(ResolveExport(kIsReadOnlySymbol));
            sendFn = reinterpret_cast<SciSendFn>(ResolveExport(kSendScintillaSymbol));
        }
        if (textFn == nullptr && sendFn == nullptr) {
            qWarning() << "[ndd-mdview] 找不到宿主 qscintilla 导出的"
                          " text / SendScintilla，预览将只能显示磁盘上的文件";
        } else {
            qWarning() << "[ndd-mdview] 已定位宿主 qscintilla 的取文入口";
        }

        QMainWindow* mw = host ? qobject_cast<QMainWindow*>(host.data()) : nullptr;
        if (mw) {
            dock = new QDockWidget(QStringLiteral("Markdown 实时预览"), mw);
            dock->setObjectName(QStringLiteral("ndd_mdview_dock"));
            panel = new MdViewPanel(dock);
            panel->setHostWidget(host);
            panel->setDockWidget(dock);
            panel->setTextSource(getCurEdit, isROFn, sendFn, textFn);
            panel->setSettings(settings);
            // setWidget 会把 panel 重新挂到 dock 内部容器上，
            // 必须在它之后才 winId()，否则原生窗口会被重建（WebView2 父句柄失效）。
            dock->setWidget(panel);
            mw->addDockWidget(Qt::RightDockWidgetArea, dock);
            panel->start(pluginDir);
            dock->hide();
            // 菜单里的勾选状态跟着面板显隐走
            if (actShow) {
                QObject::connect(dock, &QDockWidget::visibilityChanged,
                                 actShow, &QAction::setChecked);
            }
        } else {
            // 退化：不是 QMainWindow 就开独立顶层窗口
            panel = new MdViewPanel(nullptr);
            panel->setWindowFlags(Qt::Window);
            panel->setWindowTitle(QStringLiteral("Markdown 实时预览"));
            panel->resize(760, 900);
            panel->setHostWidget(host);
            panel->setTextSource(getCurEdit, isROFn, sendFn, textFn);
            panel->setSettings(settings);
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

    // 设置变了：存盘 + 让面板生效
    void applySettings()
    {
        settings.save();
        if (panel) {
            panel->setSettings(settings);
            panel->reloadPage();
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
                                                u8"支持公式（KaTeX），"
                                                u8"基于 Microsoft Edge WebView2");
    pProcData->m_version = QString::fromUtf8(u8"v1.1");
    pProcData->m_auther = QString::fromUtf8(u8"ndd-mdview");
    pProcData->m_menuType = 1;   // 二级菜单，宿主把 m_rootMenu 交给我们
    return true;
}

NDD_EXPORT int NDD_PROC_MAIN(QWidget* pNotepad,
                              const QString& strFileName,
                              std::function<QsciScintilla*(QWidget*)> getCurEdit,
                              std::function<bool(QWidget*, int, void*)> pluginCallBack,
                              NDD_PROC_DATA* pProcData)
{
    Q_UNUSED(pluginCallBack);
    // 宿主给的 getCurEdit 仿函数保留（签名是 ABI 的一部分），
    // 实测它和 Qt 对象树找到的是同一个编辑区，两个来源互相印证。
    Q_UNUSED(getCurEdit);

    // m_menuType==1：宿主建了二级菜单，句柄在 m_rootMenu。
    // 另一条路径（宿主 onPlugWork）传的是 nullptr，那种情况没法挂菜单，直接退出。
    if (pProcData == nullptr || pProcData->m_rootMenu == nullptr)
        return -1;

    MdController* ctl = new MdController(pNotepad);
    ctl->host = pNotepad;
    ctl->pluginDir = QFileInfo(strFileName).absolutePath();
    ctl->getCurEdit = getCurEdit;
    ctl->isROFn = reinterpret_cast<SciBoolFn>(ResolveExport(kIsReadOnlySymbol));
    ctl->sendFn = reinterpret_cast<SciSendFn>(ResolveExport(kSendScintillaSymbol));
    ctl->textFn = reinterpret_cast<SciTextFn>(ResolveExport(kTextSymbol));
    ctl->settings = MdSettings::load();
    g_controller = ctl;

    // --- 菜单 ---
    QMenu* menu = pProcData->m_rootMenu;
    QAction* actShow = menu->addAction(QStringLiteral("显示预览面板"));
    actShow->setCheckable(true);
    ctl->actShow = actShow;
    QObject::connect(actShow, &QAction::triggered, ctl, [ctl, actShow]() {
        // 目标状态要先记下来：ensure() 里会建面板并 hide()，
        // 那会触发 visibilityChanged 把勾选状态改回去。
        const bool want = actShow->isChecked();
        ctl->ensure();
        if (ctl->dock) {
            ctl->dock->setVisible(want);
        } else if (ctl->panel) {
            ctl->panel->setVisible(want);
        }
    });

    QAction* actRefresh = menu->addAction(QStringLiteral("立即刷新"));
    QObject::connect(actRefresh, &QAction::triggered, ctl, [ctl]() {
        ctl->ensure();
        if (ctl->panel)
            ctl->panel->refreshNow();
    });

    QMenu* menuInterval = menu->addMenu(QStringLiteral("自动更新间隔"));
    QActionGroup* group = new QActionGroup(menuInterval);
    for (int i = 0; i < kRefreshModeCount; ++i) {
        QAction* a = menuInterval->addAction(QString::fromUtf8(kRefreshLabels[i]));
        a->setCheckable(true);
        a->setChecked(ctl->settings.refreshMode == i);
        group->addAction(a);
        const int mode = i;
        QObject::connect(a, &QAction::triggered, ctl, [ctl, mode]() {
            if (ctl->settings.refreshMode == mode)
                return;
            ctl->settings.refreshMode = mode;
            ctl->applySettings();
            if (ctl->panel) {
                // 切换模式要重建定时器策略
                ctl->panel->setSettings(ctl->settings);
            }
        });
    }

    QAction* actKatex = menu->addAction(QStringLiteral("公式渲染 (KaTeX)"));
    actKatex->setCheckable(true);
    actKatex->setChecked(ctl->settings.katex);
    QObject::connect(actKatex, &QAction::triggered, ctl, [ctl, actKatex]() {
        ctl->settings.katex = actKatex->isChecked();
        ctl->applySettings();
    });

    QAction* actRelease = menu->addAction(QStringLiteral("隐藏面板时释放内存"));
    actRelease->setCheckable(true);
    actRelease->setChecked(ctl->settings.releaseOnHide);
    QObject::connect(actRelease, &QAction::triggered, ctl, [ctl, actRelease]() {
        ctl->settings.releaseOnHide = actRelease->isChecked();
        ctl->applySettings();
    });

    QAction* actReload = menu->addAction(QStringLiteral("重新加载预览页"));
    QObject::connect(actReload, &QAction::triggered, ctl, [ctl]() {
        ctl->ensure();
        if (ctl->panel)
            ctl->panel->reloadPage();
    });

    QAction* actAbout = menu->addAction(QStringLiteral("关于"));
    QObject::connect(actAbout, &QAction::triggered, ctl, [ctl]() {
        QMessageBox::information(
            ctl->host ? ctl->host.data() : nullptr,
            QStringLiteral("Markdown 实时预览 (WebView2)"),
            QStringLiteral(
                "Notepad-- 插件 · v1.1\n\n"
                "在编辑区右侧用 Microsoft Edge WebView2 实时渲染 Markdown。\n"
                "支持 GFM 表格、任务列表、围栏代码块、引用，"
                "以及 $...$ / $$...$$ 公式（KaTeX）。\n\n"
                "更新方式：默认事件驱动（编辑区一改就更新，120ms 合并），\n"
                "也可在「自动更新间隔」里改成固定轮询，适合超大文件。\n\n"
                "取文依赖宿主 qscintilla 导出的 QsciScintilla::text()。\n"
                "找不到时会退化成读磁盘文件（看不到未保存的改动）。"));
    });

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

// moc（MdViewPanel / MdController 用了 Q_OBJECT）
#include "ndd-mdview.moc"
