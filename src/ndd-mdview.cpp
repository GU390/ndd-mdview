// ============================================================================
//  ndd-mdview.cpp
//  Notepad-- 插件：内嵌 Microsoft Edge WebView2 的 Markdown 预览
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
#include <QScrollBar>
#include <QSettings>
#include <QShowEvent>
#include <QHideEvent>
#include <QResizeEvent>
#include <QDebug>
#include <QDateTime>
#include <QKeyEvent>

#include <functional>
#include <memory>
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
    bool syncScroll   = true;  // 滚动同步
    bool syncSelection = true; // 选区同步

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
        s.syncScroll = st.value(QStringLiteral("syncScroll"), true).toBool();
        s.syncSelection = st.value(QStringLiteral("syncSelection"), true).toBool();
        return s;
    }

    void save() const
    {
        QSettings st(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("Notepad--"), QStringLiteral("ndd-mdview"));
        st.setValue(QStringLiteral("refreshMode"), refreshMode);
        st.setValue(QStringLiteral("katex"), katex);
        st.setValue(QStringLiteral("releaseOnHide"), releaseOnHide);
        st.setValue(QStringLiteral("syncScroll"), syncScroll);
        st.setValue(QStringLiteral("syncSelection"), syncSelection);
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
// this=RCX，参数 EDX，无返回值
typedef void (*SciVoidIntFn)(QsciScintilla* self, int line);
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
// 滚动同步用的高层 API（比 SCI 消息干净，QScintilla 自己实现了）
static const char* kFirstVisibleLineSymbol = "?firstVisibleLine@QsciScintilla@@QEBAHXZ";
static const char* kSetFirstVisibleLineSymbol = "?setFirstVisibleLine@QsciScintilla@@QEAAXH@Z";

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

// Scintilla 消息号
// ⚠️ 必须用宿主自带的那份 Scintilla.h 里的值，不能照抄网上通用的表：
//    Notepad-- 打包的是自己的 qscintilla 分支，编号跟通用文档对不上。
//    权威来源：github.com/cxasm/notepad-- → src/qscint/scintilla/include/Scintilla.h
//    （本轮踩坑：之前写的 2304/2144/2146 全是「别的消息」——
//      2304 = SCI_CHARLEFT（光标左移一格），不是 SCI_SETSEL；
//      2144 = SCI_SETSELECTIONEND（设置！），不是 SCI_GETSELECTIONEND；
//      2146 = SCI_SETPRINTMAGNIFICATION，不是 SCI_LINEFROMPOSITION。
//      症状是「日志说选中了，其实一个字都没选上」，而且每 100ms 还会
//      把用户自己在编辑区拉的选区终点改到 0。）
#define SCI_GETLENGTH      2006
#define SCI_GETTEXT        2182
#define SCI_GETCODEPAGE    2137
#define SCI_GETFIRSTVISIBLELINE 2152
#define SCI_GETLINECOUNT     2154
#define SCI_GETSELECTIONSTART 2143
#define SCI_GETSELECTIONEND   2145
#define SCI_LINEFROMPOSITION  2166
#define SCI_POSITIONFROMLINE  2167
#define SCI_SETSEL            2160
#define SCI_ENSUREVISIBLE     2232   // 把光标最小幅度滚进视野（不会跳）
#define SCI_SETREADONLY       2171   // 设只读（宿主 Scintilla.h 的值，测试钩子用）
// 这版 Scintilla 已经没有 SCI_LINESCROLLTO 了（2168=SCI_LINESCROLL, 2169=SCI_SCROLLCARET），
// 所以滚动只能用 QScintilla 的 firstVisibleLine()/setFirstVisibleLine()。

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
    // ⚠️ 空文档时宿主给回来的是「共享空串」（QArrayData::shared_null），
    //    那是全局静态对象，refcount 初始是 -1；
    //    去析构它会把那个静态的 refcount 减成 -2，属于悄悄改坏全局状态。
    //    所以只在拿到真正的独立数据时才析构。
    if (!q->isNull())
        q->~QString();      // 释放槽里那个
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
static const char* kGithubCssResource = ":/mdview/github-markdown.css";
static const char* kKatexJsResource    = ":/mdview/katex.min.js";
static const char* kKatexCssResource   = ":/mdview/katex.min.css";
static const char* kKatexFontResource  = ":/mdview/fonts/";
static const char* kKatexMarker        = "assets.ok";

// 临时目录：%TEMP%\ndd-mdview\web
static QString WebAssetDir()
{
    return QDir(QDir::tempPath())
        .filePath(QStringLiteral("ndd-mdview") + QDir::separator()
                  + QStringLiteral("web"));
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

// 解包网页要用的静态资源（幂等：靠标记文件判断是否已解过）
//   github-markdown.css —— 现成的成熟排版（MIT），不再手写
//   katex.min.js / katex.min.css / fonts\*.woff2 —— 公式排版
static bool ExtractWebAssets(bool withKatex, QString* err)
{
    const QString dir = WebAssetDir();
    QDir d(dir);
    if (!d.exists() && !d.mkpath(QStringLiteral("."))) {
        *err = QStringLiteral("无法创建临时目录：%1").arg(dir);
        return false;
    }

    const QString marker = d.filePath(QStringLiteral("assets.ok"));
    if (QFile::exists(marker))
        return true;

    if (!WriteResourceToFile(QString::fromLatin1(kGithubCssResource),
                             d.filePath(QStringLiteral("github-markdown.css")))) {
        *err = QStringLiteral("解包 github-markdown.css 失败");
        return false;
    }

    if (withKatex) {
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

class WebMessageHandler final
    : public ICoreWebView2WebMessageReceivedEventHandler
{
public:
    typedef std::function<void(const QString&)> Fn;

    explicit WebMessageHandler(Fn fn) : m_fn(std::move(fn)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(ICoreWebView2WebMessageReceivedEventHandler)) {
            *ppv = static_cast<ICoreWebView2WebMessageReceivedEventHandler*>(this);
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

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*,
                                     ICoreWebView2WebMessageReceivedEventArgs* args) override
    {
        LPWSTR msg = nullptr;
        if (args != nullptr && SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg != nullptr) {
            const QString s = QString::fromWCharArray(msg);
            CoTaskMemFree(msg);
            m_fn(s);
        }
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
        // 调试钩子 NDD_MDVIEW_TESTRO：
        //   1    -> 把当前编辑区设成只读（SCI_SETREADONLY），走 v1.8 新逻辑（跳过宿主回调）
        //   hostcall -> 同样设成只读，但强制调用宿主回调（= v1.7 旧行为），用于 A/B 对照
        // 为什么需要它：普通 .md 打开在宿主里**不是**只读的
        // （openTextFile 里 isReadOnly 恒为 false），只有「超大文件只读模式」和
        // 「tail 监控模式」才会只读 —— 前者要 >100MB 文件 + 弹窗，后者要点菜单。
        // SCI_SETREADONLY 走的正是宿主 setReadOnly(true) 那条路，所以等价。
        const QByteArray testro = qgetenv("NDD_MDVIEW_TESTRO");
        m_testRO = (!testro.isEmpty() && testro != "0");
        m_testForceHostCall = (testro == "hostcall");
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

    // 设置变了：更新定时器策略（连不连信号）、标题，公式开关变了就重载页面
    void applySettings(const MdSettings& s)
    {
        const bool needReload = (m_settings.katex != s.katex);
        m_settings = s;
        applyRefreshMode();
        updateTitle();
        if (needReload)
            reloadPage();
    }

    // 状态显示在停靠窗格标题上，方便一眼看出卡在哪一步
    void setStatus(const QString& s)
    {
        m_status = s;
        if (m_dock) {
            m_dock->setWindowTitle(m_status.isEmpty()
                                       ? QStringLiteral("Markdown 预览")
                                       : QStringLiteral("Markdown 预览 | ") + m_status);
        }
    }

    // 初始化（异步创建 WebView2）。返回 false 表示立即失败。
    bool start(const QString& pluginDir);

    // 立即刷新一次（菜单用）
    void refreshNow();

    // 自测：把编辑区滚到某行，再读回网页当前的行号（验证滚动同步）
    void testScrollSync(int line);

    // 自测：在编辑区里选中 l1..l2 行（验证选区同步）
    void testSelectionSync(int l1, int l2);

    // 自测：在预览里选中第 blockIndex 个块（验证「预览选区 -> 编辑区选中」）
    void testWebSelectionSync(int blockIndex);

    // 自测：核对「第 N 行」在编辑区和预览里是不是同一段内容（行号映射）
    void testLineMapping(int line);

    // 自测：在网页里滚到第 line 行（模拟「用户自己滚网页」），宿主应把编辑区也滚过去
    void testWebScroll(int line);

    // 页面是否就绪（自测钩子要等它，否则推送根本发不出去）
    bool isPageReady() const { return m_pageReady; }

    // 页面报 'R'（内容渲染完）时通知外部（用来触发待跑的自测）
    void setContentReadyCallback(std::function<void()> cb) { m_contentReadyCb = std::move(cb); }

    // 把渲染结果统计回读到日志（无头验证用：katex 元素数、公式出错数、正文长度）
    void logRenderStats();

    // 设置变了：重新装载页面
    void reloadPage();

    // 隐藏时按设置释放 WebView2 内存
    void releaseWebView();

public slots:
    // 编辑区内容变了（事件驱动路径）
    void onEditorTextChanged();
    // 标签页切换了
    void onTabCurrentChanged(int index);
    // 网页滚动了（预览 -> 编辑区）
    void onWebScroll(int line);
    // 网页里选了内容（预览 -> 编辑区）
    void onWebSelection(int l1, int l2);

protected:
    void resizeEvent(QResizeEvent* e) override
    {
        QWidget::resizeEvent(e);
        syncBounds();
    }

    void showEvent(QShowEvent* e) override
    {
        QWidget::showEvent(e);

        HWND cur = reinterpret_cast<HWND>(winId());
        if (!m_view && m_env) {
            // 上次隐藏时按设置释放了 WebView2（省内存），这里重新建一个
            m_hwnd = cur;
            m_pageReady = false;
            createController();
        } else if (m_view && cur != m_hwnd) {
            // 停靠位置变化 / 浮动 / 还原 时，Qt 可能重建原生窗口，
            // 那样 WebView2 的父句柄就失效了 —— 检测到变化就重建控制器。
            m_hwnd = cur;
            m_ctrl.Reset();
            m_view.Reset();
            m_pageReady = false;
            createController();
        }

        if (m_poll)
            m_poll->start();
        if (m_scrollPoll)
            m_scrollPoll->start();
        refreshNow();

        // 布局可能还没稳定，过一会儿再对一次 bounds、并补一次推送
        QTimer::singleShot(0, this, [this]() { syncBounds(); });
        QTimer::singleShot(400, this, [this]() {
            syncBounds();
            m_lastText.clear();
            pushCurrentText();
            updateTitle();
        });
    }

    void hideEvent(QHideEvent* e) override
    {
        if (m_poll)
            m_poll->stop();
        if (m_coalesce)
            m_coalesce->stop();
        if (m_scrollPoll)
            m_scrollPoll->stop();
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
    void updateSignalConnection();
    QString modeLabel() const;
    void updateTitle();
    bool editorSelectionLines(int* l1, int* l2, bool* hasRange = nullptr);
    void syncSelectionFromEditor();

    // --- 滚动同步 ---
    void setupScrollSync(QWidget* editor);
    int  editorFirstVisibleLine();
    int  editorLineCount();
    void scrollEditorToLine(int line);
    // 只做**最小滚动**：保证选区可见，但不把编辑区拽到区间第一行。
    // （预览回传的是「整块」的行号范围，滚到区间头就是用户说的「跳 50~70 行」）
    void ensureEditorSelectionVisible();
    void syncScrollFromEditor();

    QPointer<QWidget> m_host;
    QPointer<QDockWidget> m_dock;
    // 页面报 'R'（内容渲染完）时回调控制器 —— 用回调而不是指针，
    // 面板就不需要知道控制器的完整类型。
    std::function<void()> m_contentReadyCb;
    std::function<QsciScintilla*(QWidget*)> m_getCurEdit;
    SciBoolFn m_isROFn = nullptr;
    SciSendFn m_sendFn = nullptr;
    SciTextFn m_textFn = nullptr;
    char* m_buf = nullptr;
    int m_bufCap = 0;
    // ⚠️ 这几个编辑区指针必须是 QPointer，不能是裸 QWidget*：
    //    100ms 的滚动/选区轮询一直在用它们，而关掉标签页时宿主会
    //    销毁 ScintillaEditView —— 裸指针就变成悬垂指针，
    //    SendScintilla 会去已释放的内存上取 vtable（SEH 拦不住这种
    //    use-after-free，崩在 Qt5Core 里，症状是「莫名其妙崩了」）。
    QPointer<QWidget> m_lastEditor;
    QPointer<QWidget> m_connEditor;
    QPointer<QWidget> m_connTab;
    bool m_signalConnected = false;
    // 滚动同步
    QPointer<QWidget> m_scrollEditor;
    SciIntFn m_firstLineFn = nullptr;
    SciVoidIntFn m_setFirstLineFn = nullptr;
    QTimer* m_scrollPoll = nullptr;
    int m_lastEditorLine = -1;
    int m_lastWebLine = -1;
    int m_lastSelL1 = 0;
    int m_lastSelL2 = 0;
    // 推给网页的选区区间（1 基；用来认「回声」）
    int m_pushedSelL1 = 0;
    int m_pushedSelL2 = 0;
    qint64 m_scrollSuppressUntil = 0;
    // 滚动回声判定（要精确，见 onWebScroll）：
    //   上次推给网页的目标行 + 推送时刻；网页滚完报回来的行跟它对得上 => 是回声。
    int  m_pushedScrollLine = -1;
    qint64 m_lastScrollPushAt = 0;
    // 「选区推给网页」引起的网页滚动，不许回弹编辑区（否则选一下视口就跳）。
    qint64 m_selScrollSuppressUntil = 0;
    // 选区同步的抑制窗口必须**独立**于滚动的那个：
    // 用户在预览里点一下/拖一下，浏览器往往同时发滚动事件（光标滚进视野），
    // 'L' 先到就把 m_scrollSuppressUntil 撑开，随后到的 'S'（选区）
    // 会被同一个窗口一起误杀 —— 表现就是「在右边选了，左边不动」。
    qint64 m_selSuppressUntil = 0;
    bool m_probed = false;
    bool m_routeLogged = false;
    // v1.8：只读文档时跳过宿主 getCurEdit 回调（否则宿主会响铃），换文档后允许再提示一次
    bool m_roSkipLogged = false;
    // 调试钩子（见构造函数）：模拟只读文档
    bool m_testRO = false;
    bool m_testRODone = false;
    bool m_testForceHostCall = false;
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
    bool m_symbolsResolved = false;  // 导出符号是否已解析过
    qint64 m_lastStatsMs = 0;        // 渲染统计的节流时间戳
};

// --- 面板实现 ---------------------------------------------------------------

void MdViewPanel::applyRefreshMode()
{
    // 信号连不连，取决于当前模式
    updateSignalConnection();

    if (!m_poll)
        return;
    const int interval = kRefreshIntervals[m_settings.refreshMode];
    if (interval <= 0) {
        // 实时模式：主要靠编辑区的 textChanged 信号（改一个字 120ms 内就推）。
        // 但仍留一个 1 秒的看门狗：
        //   · 编辑区可能是插件加载之后才建好的（那时还没接上信号）
        //   · 切换标签页 / 换文件时对象会变
        // 看门狗只做「重新定位 + 比对文本」，文本没变就不推送，
        // 所以不会产生 markdown-it 重渲染的开销。
        m_poll->setInterval(1000);
    } else {
        m_poll->setInterval(interval);
    }
    if (isVisible())
        m_poll->start();
}

// 当前更新模式的文字（显示在停靠窗格标题上）
QString MdViewPanel::modeLabel() const
{
    const int i = m_settings.refreshMode;
    if (i <= 0)
        return QStringLiteral("实时");
    const int ms = kRefreshIntervals[i];
    if (ms < 1000)
        return QStringLiteral("每 %1 毫秒").arg(ms);
    return QStringLiteral("每 %1 秒").arg(ms / 1000);
}

// 标题统一格式：Markdown 预览 | 模式 | 字符数
void MdViewPanel::updateTitle()
{
    if (!m_dock)
        return;
    m_dock->setWindowTitle(QStringLiteral("Markdown 预览 | %1 | %2 字符")
                            .arg(modeLabel())
                            .arg(m_currentText.size()));
}

void MdViewPanel::connectEditor(QWidget* ed)
{
    if (m_connEditor.data() == ed) {
        updateSignalConnection();
        return;
    }

    if (!m_connEditor.isNull())
        m_connEditor.data()->disconnect(this);
    m_connEditor = ed;
    m_signalConnected = false;
    updateSignalConnection();

    // 标签页切换时换编辑区
    QWidget* tab = FindTabAncestor(ed);
    if (m_connTab.data() != tab) {
        if (!m_connTab.isNull())
            m_connTab.data()->disconnect(this);
        m_connTab = tab;
        if (tab)
            QObject::connect(tab, SIGNAL(currentChanged(int)), this, SLOT(onTabCurrentChanged(int)));
    }
}

// 「实时」模式才连 textChanged 信号（事件驱动）；
// 选了固定间隔就断开信号，改为纯定时轮询 —— 否则用户设了 5 秒也照样实时刷。
void MdViewPanel::updateSignalConnection()
{
    const bool want = (m_settings.refreshMode == 0) && (!m_connEditor.isNull());
    if (want == m_signalConnected)
        return;
    if (m_signalConnected && !m_connEditor.isNull())
        m_connEditor.data()->disconnect(this);
    m_signalConnected = false;
    if (want) {
        // 字符串式连接：不需要 QsciScintilla 的类定义。
        // QsciScintilla 有 textChanged() 信号（宿主自己就用它连 Markdown 预览）。
        QObject::connect(m_connEditor.data(), SIGNAL(textChanged()), this, SLOT(onEditorTextChanged()));
        m_signalConnected = true;
    }
}

void MdViewPanel::setupScrollSync(QWidget* editor)
{
    if (m_scrollEditor.data() == editor)
        return;

    m_scrollEditor = editor;
    m_lastEditorLine = -1;
    m_lastWebLine = -1;

    if (editor == nullptr)
        return;

    // 用 qscintilla 自己的高层 API 读/写滚动位置：
    //   int  firstVisibleLine() const
    //   void setFirstVisibleLine(int)
    // ⚠️ QScintilla 并不用 QAbstractScrollArea 那个滚动条（实测 setValue 改了值
    //    但编辑区不滚），所以别去碰 findChild<QScrollBar*>()。
    // 检测用 100ms 轮询一次「首行」，比每次复制整篇文档便宜得多。
    if (!m_scrollPoll) {
        m_scrollPoll = new QTimer(this);
        m_scrollPoll->setInterval(100);
        QObject::connect(m_scrollPoll, &QTimer::timeout, this, [this]() {
            syncScrollFromEditor();
            syncSelectionFromEditor();
        });
    }
    if (isVisible())
        m_scrollPoll->start();
    qWarning() << "[ndd-mdview] 滚动同步: 编辑区首行 =" << editorFirstVisibleLine()
               << " 总行数 =" << editorLineCount();
}

int MdViewPanel::editorFirstVisibleLine()
{
    if (m_firstLineFn == nullptr || m_scrollEditor.isNull())
        return -1;
    int line = -1;
    __try {
        line = m_firstLineFn(reinterpret_cast<QsciScintilla*>(m_scrollEditor.data()));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return line;
}

int MdViewPanel::editorLineCount()
{
    if (m_sendFn == nullptr || m_scrollEditor.isNull())
        return -1;
    int n = -1;
    __try {
        n = (int)m_sendFn(reinterpret_cast<QsciScintilla*>(m_scrollEditor.data()),
                          SCI_GETLINECOUNT, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return n;
}

void MdViewPanel::scrollEditorToLine(int line)
{
    if (m_setFirstLineFn == nullptr || m_scrollEditor.isNull() || line < 0)
        return;
    __try {
        m_setFirstLineFn(reinterpret_cast<QsciScintilla*>(m_scrollEditor.data()), line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void MdViewPanel::syncScrollFromEditor()
{
    if (!m_view || !m_pageReady || !m_settings.syncScroll)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now < m_scrollSuppressUntil)     // 正在做同步，忽略回声
        return;
    const int line = editorFirstVisibleLine();
    if (line < 0 || line == m_lastEditorLine)
        return;
    m_lastEditorLine = line;
    m_scrollSuppressUntil = now + 250;
    qWarning() << "[ndd-mdview] 滚动同步: 编辑区首行 ->" << line << "，推给网页";
    // data-line 是 1 基，Scintilla 行号是 0 基
    const int target = line + 1;
    m_pushedScrollLine = target;        // 记住推了哪一行，回声靠它认
    m_lastScrollPushAt = now;
    const QString js = QStringLiteral("window.mdScrollToLine(%1);").arg(target);
    m_view->ExecuteScript(reinterpret_cast<LPCWSTR>(js.utf16()), nullptr);
}

void MdViewPanel::onWebScroll(int line)
{
    if (line <= 0 || !m_settings.syncScroll)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // ⚠️ 回声判定必须**精确**，不能用「一段时间内一律忽略」。
    //    网页滚完会报一次当前顶行；那行跟我们刚推过去的目标一致 => 是回声，丢掉。
    //    之前用的是「250ms 内的滚动一律忽略」—— 结果把**用户自己滚网页**也吞了，
    //    两边的视口就此错开而且再也回不来（用户看到的「左右不同步」）。
    if (m_pushedScrollLine > 0 && now - m_lastScrollPushAt < 2000 &&
        line >= m_pushedScrollLine - 3 && line <= m_pushedScrollLine + 3) {
        return;                          // 回声，忽略（保留 m_pushedScrollLine）
    }
    m_pushedScrollLine = -1;             // 不是回声 => 认定是用户滚的
    if (now < m_selScrollSuppressUntil)  // 选区推送引起的滚动，不许回弹编辑区
        return;
    if (line == m_lastWebLine)
        return;
    m_lastWebLine = line;
    m_scrollSuppressUntil = now + 250;
    qWarning() << "[ndd-mdview] 滚动同步: 网页行 ->" << line << "，滚编辑区";
    scrollEditorToLine(line - 1);
}

// ---------------------------------------------------------------------
//  选区同步
// ---------------------------------------------------------------------

// 编辑区当前选区的起止行号（1 基；没有选区返回 false）
// hasRange：有真实选区（不是只有一个光标）时为 true。
//   只有光标时不往预览里做原生选区 —— 否则用户每敲一行，
//   预览的选区就跟着跳一下，页面还会乱滚。
bool MdViewPanel::editorSelectionLines(int* l1, int* l2, bool* hasRange)
{
    if (m_sendFn == nullptr || m_scrollEditor.isNull())
        return false;
    int start = -1, end = -1, ls = -1, le = -1;
    __try {
        start = (int)m_sendFn(reinterpret_cast<QsciScintilla*>(m_scrollEditor.data()),
                              SCI_GETSELECTIONSTART, 0, 0);
        end = (int)m_sendFn(reinterpret_cast<QsciScintilla*>(m_scrollEditor.data()),
                            SCI_GETSELECTIONEND, 0, 0);
        ls = (int)m_sendFn(reinterpret_cast<QsciScintilla*>(m_scrollEditor.data()),
                           SCI_LINEFROMPOSITION, (unsigned long)start, 0);
        le = (int)m_sendFn(reinterpret_cast<QsciScintilla*>(m_scrollEditor.data()),
                           SCI_LINEFROMPOSITION, (unsigned long)end, 0);
        // 选区终点正好落在下一行行首时（用户拖到行首就松手），
        // 实际覆盖到的是上一行行尾，这里把 le 收回来，别多高亮一个块。
        if (end > start && le > ls) {
            int lineStart = (int)m_sendFn(reinterpret_cast<QsciScintilla*>(m_scrollEditor.data()),
                                          SCI_POSITIONFROMLINE, (unsigned long)le, 0);
            if (lineStart == end)
                le -= 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (ls < 0 || le < ls)
        return false;
    *l1 = ls + 1;
    *l2 = le + 1;
    if (hasRange)
        *hasRange = (end > start);
    return true;
}

// 编辑区选区变了 -> 高亮预览里相交的块，并在预览里做真正的原生选区
// 最小滚动：让选区可见，但视口尽量不动。
// 用 SCI_ENSUREVISIBLE 而不是 setFirstVisibleLine(区间头) ——
// 后者会把编辑区直接拽到区间第一行（一个 60 行的代码块 = 跳 60 行）。
void MdViewPanel::ensureEditorSelectionVisible()
{
    if (m_sendFn == nullptr || m_scrollEditor.isNull())
        return;
    __try {
        m_sendFn(reinterpret_cast<QsciScintilla*>(m_scrollEditor.data()),
                 SCI_ENSUREVISIBLE, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void MdViewPanel::syncSelectionFromEditor()
{
    if (!m_view || !m_pageReady || !m_settings.syncSelection)
        return;
    int l1 = 0, l2 = 0;
    bool hasRange = false;
    if (!editorSelectionLines(&l1, &l2, &hasRange)) {
        if (m_lastSelL1 != 0 || m_lastSelL2 != 0) {
            m_lastSelL1 = 0;
            m_lastSelL2 = 0;
            m_view->ExecuteScript(L"window.mdSyncSelection && window.mdSyncSelection(0,0,false);", nullptr);
        }
        return;
    }
    if (l1 == m_lastSelL1 && l2 == m_lastSelL2)
        return;
    m_lastSelL1 = l1;
    m_lastSelL2 = l2;
    qWarning() << "[ndd-mdview] 选区同步: 编辑区选区 ->" << l1 << ".." << l2
               << (hasRange ? "（有选区）" : "（仅光标）") << "，推给网页";
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // 自己推过去的选区，页面会再发一个 'S' 回来 —— 这是回声，要挡掉。
    // 靠**内容**认，不靠时间：回声报回来的区间是「包含」我们推的区间的那一块
    // （页面里选区是按块做的），所以「包含 ⇒ 回声」。
    // 时间窗口两头都不准：回声迟到会被放行（编辑区被拽到块首），
    // 用户在窗口内真的选了预览又会被误杀。
    m_pushedSelL1 = qMin(l1, l2);
    m_pushedSelL2 = qMax(l1, l2);
    m_selSuppressUntil = now + 1500;
    // 页面如果因为设了选区而滚动一下，那一下也不能回弹编辑区 ——
    // 否则「选一下就跳几十行」（网页视口高度 ≈ 50~70 行）。
    m_selScrollSuppressUntil = now + 400;
    const QString js = QStringLiteral("window.mdSyncSelection && window.mdSyncSelection(%1,%2,%3);")
        .arg(l1).arg(l2).arg(hasRange ? QStringLiteral("true") : QStringLiteral("false"));
    m_view->ExecuteScript(reinterpret_cast<LPCWSTR>(js.utf16()), nullptr);
}

// 在编辑区里选中 l1..l2 行（1 基）。
// 单独拆出来：带 __try 的函数里不能有需要回卷的 C++ 对象（MSVC 报 C2712），
// 所以这个函数里只准出现指针和 int。
// ⚠️ 发完 SCI_SETSEL 必须回读确认：消息发出去 ≠ 宿主执行了。
//    之前用错的消息号（2304=SCI_CHARLEFT）就是这样「静默失败」的，
//    而调用方只看见 return true，日志还照样打「已选中」。
static bool SciSelectLines(SciSendFn send, QsciScintilla* ed, int l1, int l2,
                            int* outStart, int* outEnd)
{
    __try {
        int start = (int)send(ed, SCI_POSITIONFROMLINE, (unsigned long)(l1 - 1), 0);
        int end = (int)send(ed, SCI_POSITIONFROMLINE, (unsigned long)l2, 0) - 1;
        if (end < start)
            end = start;
        send(ed, SCI_SETSEL, (unsigned long)start, (long)end);
        // 回读：只有真的选上了才算成功
        int gs = (int)send(ed, SCI_GETSELECTIONSTART, 0, 0);
        int ge = (int)send(ed, SCI_GETSELECTIONEND, 0, 0);
        if (outStart)
            *outStart = gs;
        if (outEnd)
            *outEnd = ge;
        return (gs == start && ge == end);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 预览里选了内容 -> 编辑区选中对应的行
void MdViewPanel::onWebSelection(int l1, int l2)
{
    if (!m_settings.syncSelection || l1 <= 0 || l2 < l1)
        return;
    if (m_sendFn == nullptr || m_scrollEditor.isNull())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // ⚠️ 这里**不能**查 m_scrollSuppressUntil：
    //    用户在预览里拖选时，浏览器常常同时发滚动事件（光标滚进视野），
    //    'L' 先到会把滚动窗口撑开，随后到的 'S' 就被一起误杀了 ——
    //    表现就是「右边选了，左边不动」。选区要用自己的回声窗口。
    // 回声判定（按内容，不按时间）：
    // 网页回传的区间**包含**我们推给网页的那一段 ⇒ 是我们自己设选区引起的回声 ⇒ 丢掉。
    // （用户真去选同一个块的情况也无所谓 —— 结果一样。）
    if (m_pushedSelL1 > 0 &&
        l1 <= m_pushedSelL1 && l2 >= m_pushedSelL2) {
        qWarning() << "[ndd-mdview] 选区同步: 网页行" << l1 << ".." << l2
                   << " 是本插件刚推过去的选区的回声，忽略";
        return;
    }
    int gs = -1, ge = -1;
    if (!SciSelectLines(m_sendFn, reinterpret_cast<QsciScintilla*>(m_scrollEditor.data()), l1, l2,
                        &gs, &ge)) {
        qWarning() << "[ndd-mdview] 选区同步: 网页行" << l1 << ".." << l2
                   << "-> 编辑区选中失败（回读 gs=" << gs << " ge=" << ge << "）";
        return;
    }
    m_scrollSuppressUntil = now + 250;
    // 让选区可见，但**最小幅度**地滚 —— 不能滚到区间第一行：
    // 预览回传的是「整块」的行号范围（一个 60 行代码块就是 110..171），
    // 滚到 110 正是用户报的「171 跳到 110」。
    ensureEditorSelectionVisible();
    qWarning() << "[ndd-mdview] 选区同步: 网页行 " << l1 << ".." << l2
               << " -> 编辑区选中（位置 " << gs << ".." << ge << "，回读确认）";
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
    // 换标签页时旧编辑区可能已经被销毁（QPointer 会自动置空），
    // 这里把滚动/选区同步的基准也清掉，下一轮重新绑定到新编辑区。
    m_scrollEditor = nullptr;
    m_lastEditorLine = -1;
    m_lastWebLine = -1;
    m_lastSelL1 = 0;
    m_lastSelL2 = 0;
    m_lastText.clear();
    refreshNow();
}

void MdViewPanel::refreshNow()
{
    pollNow();
}

QString MdViewPanel::buildHtml(const QString& mdJs) const
{
    // 排版用现成的 github-markdown.css（MIT，sindresorhus），
    // 自己只补一点滚动条 / 选区高亮 / 公式的微调。
    static const char* kCss =
        "html,body{margin:0;padding:0;background:#fff;}"
        "body{font-family:'Microsoft YaHei','Segoe UI',system-ui,sans-serif;}"
        "#md{padding:24px 32px 80px 32px;box-sizing:border-box;word-wrap:break-word;}"
        "#md::-webkit-scrollbar{width:10px;}"
        "#md::-webkit-scrollbar-thumb{background:#c8c8c8;border-radius:5px;}"
        /* 选区高亮（编辑区选过来的） */
        "#md .md-sel{background:#fff3c4 !important;border-radius:3px;"
        "box-shadow:0 0 0 2px #fff3c4;}"
        /* 公式：KaTeX 排版需要，允许横向滚动 */
        "#md .katex-display{margin:1em 0;overflow-x:auto;overflow-y:hidden;}"
        "#md .katex{font-size:1.05em;}"
        /* 公式里夹中文：KaTeX 自带字体没有汉字，靠浏览器逐字回退 */
        "#md .katex .mord.text,#md .katex .text{font-family:inherit;}"
        /* 公式语法错误时退回显示原始 LaTeX */
        "#md code.md-math-error{color:#b42318;background:#fff5f5;"
        "border:1px solid #f3c0c0;padding:2px 4px;border-radius:4px;}";

    // JS 侧：window.mdRender(text) -> 渲染
    static const char* kApi =
        "(function(){"
        "  var md = window.markdownit({html:false,linkify:true,breaks:false,"
        "typographer:false,xhtmlOut:false});"
        "  if (window.katex && window.mdviewMath) {"
        "    try { md.use(window.mdviewMath, {throwOnError:false,strict:false,trust:false}); }"
        "    catch(e) { console.log('math plugin failed', e); }"
        "  }"
        /* 给块级元素加 data-line / data-line2：markdown-it 的 block token 带
           token.map（起止行号），有了它就能做「编辑区 <-> 预览」的
           滚动同步与选区高亮。 */
        "  (function(){"
        "    var names = ['paragraph_open','heading_open','blockquote_open',"
        "'list_item_open','table_open','tr_open','fence','code_block','hr',"
        "'html_block','math_block'];"
        "    var rules = md.renderer.rules;"
        "    for (var i = 0; i < names.length; i++) {"
        "      (function(n){"
        "        var orig = rules[n];"
        "        rules[n] = function(tokens, idx, opts, env, self){"
        "          var token = tokens[idx];"
        "          var html = orig ? orig(tokens, idx, opts, env, self)"
        "                           : self.renderToken(tokens, idx, opts);"
        "          if (token && token.map) {"
        "            var s = token.map[0] + 1, e = token.map[1] + 1;"
        "            html = html.replace(/^<([a-zA-Z][a-zA-Z0-9]*)/,"
        "                             '<$1 data-line=\"' + s + '\" data-line2=\"' + e + '\"');"
        "          }"
        "          return html;"
        "        };"
        "      })(names[i]);"
        "    }"
        "  })();"
        /* 内容渲染完成 -> 告诉宿主一声（自测钩子要等这个） */
        "  window.mdContentReady = function(){"
        "    if (window.mdContentReadyDone) { return; }"
        "    window.mdContentReadyDone = true;"
        "    try { window.chrome.webview.postMessage('R'); } catch(e){}"
        "  };"
        "  window.mdRender = function(text){"
        "    try{"
        "      document.getElementById('md').innerHTML = md.render(text);"
        "      window.mdContentReady && window.mdContentReady();"
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
        /* --- 滚动同步 --- */
        "  function lineEls(){ return document.querySelectorAll('#md [data-line]'); }"
        /* 行号 -> 滚动到对应块 */
        "  window.mdScrollToLine = function(line){"
        "    var els = lineEls(), best = null, bestLine = -1, i, l;"
        "    for (i = 0; i < els.length; i++){"
        "      l = parseInt(els[i].getAttribute('data-line'), 10);"
        "      if (!isNaN(l) && l <= line && l > bestLine){ bestLine = l; best = els[i]; }"
        "    }"
        "    var top = (window.pageYOffset || document.documentElement.scrollTop || 0);"
        "    var y = best ? (best.getBoundingClientRect().top + top - 6) : 0;"
        "    window.scrollTo(0, Math.max(0, y));"
        "  };"
        /* 当前视口顶部的行号（预览 -> 编辑区） */
        "  window.mdCurrentLine = function(){"
        "    var els = lineEls(), line = 1, i, r, l;"
        "    for (i = 0; i < els.length; i++){"
        "      r = els[i].getBoundingClientRect();"
        "      if (r.bottom < 4){"
        "        l = parseInt(els[i].getAttribute('data-line'), 10);"
        "        if (!isNaN(l)) line = l;"
        "      } else break;"
        "    }"
        "    return line;"
        "  };"
        /* --- 选区同步 --- */
        /* 编辑区选中了 s..e 行 -> 高亮预览里相交的块 */
        "  window.mdHighlight = function(s, e){"
        "    var els = lineEls(), i, a, b, hit;"
        "    for (i = 0; i < els.length; i++){"
        "      a = parseInt(els[i].getAttribute('data-line'), 10);"
        "      b = parseInt(els[i].getAttribute('data-line2'), 10) || a;"
        "      hit = (a <= e && b >= s);"
        "      if (hit) { els[i].classList.add('md-sel'); }"
        "      else { els[i].classList.remove('md-sel'); }"
        "    }"
        "  };"
        /* 编辑区选中了 s..e 行 -> 预览里做**真正的原生选区**（这样左右两边
           选中的文字才真的对上，而不是只给整块加个黄底）。
           doSelect=false（编辑区只有一个光标时）只高亮整块，
           不动预览的原生选区 —— 否则用户每敲一行，预览就跟着跳一下。 */
        "  window.mdSyncSelection = function(s, e, doSelect){"
        "    if (!s || !e) { window.mdHighlight && window.mdHighlight(0,0); return 'clear'; }"
        "    var els = lineEls(), i, a, b, first = null, last = null;"
        "    for (i = 0; i < els.length; i++){"
        "      a = parseInt(els[i].getAttribute('data-line'), 10);"
        "      b = parseInt(els[i].getAttribute('data-line2'), 10) || a;"
        "      if (a <= e && b >= s){ if (!first) { first = els[i]; } last = els[i]; }"
        "    }"
        "    window.mdHighlight && window.mdHighlight(s, e);"
        "    if (!doSelect || !first || !last) { return 'highlight-only'; }"
        "    try {"
        "      var r = document.createRange();"
        "      if (first.childNodes.length) { r.setStart(first, 0); } else { r.setStartBefore(first); }"
        "      if (last.childNodes.length) { r.setEnd(last, last.childNodes.length); } else { r.setEndAfter(last); }"
        "      var sel = window.getSelection();"
        "      sel.removeAllRanges();"
        "      sel.addRange(r);"
        "      /* 这里**故意不 scrollIntoView**："
        "         滚动位置由滚动同步负责保持两边对齐；"
        "         在这里滚会把网页视口拉歪，网页再把顶行报回来，"
        "         编辑区就被带着跳一整个视口（50~70 行）。 */"
        "      return 'ok';"
        "    } catch(e){ return 'err:' + e; }"
        "  };"
        /* 预览里选中了内容 -> 返回对应的行号区间 */
        "  window.mdSelectionLines = function(){"
        "    var sel = window.getSelection();"
        "    if (!sel || sel.rangeCount === 0 || sel.isCollapsed) { return null; }"
        "    var range = sel.getRangeAt(0), els = lineEls(), i, el, s = null, e = null;"
        "    for (i = 0; i < els.length; i++){"
        "      el = els[i];"
        "      if (el.contains(range.startContainer) || el.contains(range.endContainer)){"
        "        if (s === null){ s = parseInt(el.getAttribute('data-line'), 10); }"
        "        e = parseInt(el.getAttribute('data-line2'), 10) || s;"
        "      }"
        "    }"
        "    return (s === null) ? null : (s + ',' + e);"
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
        /* 预览滚动 -> 通知宿主（80ms 节流） */
        "      var last = 0;"
        "      window.addEventListener('scroll', function(){"
        "        var now = Date.now();"
        "        if (now - last < 80) { return; }"
        "        last = now;"
        "        try { window.chrome.webview.postMessage('L' + window.mdCurrentLine()); }"
        "        catch(e){}"
        "      }, {passive:true});"
        /* 预览里选中内容 -> 通知宿主（150ms 节流） */
        "      var lastSel = 0;"
        "      document.addEventListener('selectionchange', function(){"
        "        var now = Date.now();"
        "        if (now - lastSel < 150) { return; }"
        "        lastSel = now;"
        "        var lines = window.mdSelectionLines();"
        "        if (lines){"
        "          try { window.chrome.webview.postMessage('S' + lines); } catch(e){}"
        "        }"
        "      });"
        "    }"
        "  }catch(e){ window.mdvSupportsPost = false; }"
        "})();";

    // 样式表：github-markdown.css（现成的成熟排版）+ KaTeX 的 css（开公式时）
    QString katexHead;
    QString katexTail;
    katexHead = QStringLiteral(
        "<link rel=\"stylesheet\" href=\"https://mdview.local/github-markdown.css\">");
    if (m_settings.katex) {
        katexHead += QStringLiteral(
            "<link rel=\"stylesheet\" href=\"https://mdview.local/katex.min.css\">");
    }
    if (m_settings.katex) {
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
    html += QStringLiteral("<div id=\"md\" class=\"markdown-body\"></div>");
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

    // 网页资源（github-markdown.css / KaTeX）：解包到 %TEMP%，再用虚拟主机映射暴露
    {
        QString err;
        if (!ExtractWebAssets(m_settings.katex, &err)) {
            qWarning() << "[ndd-mdview]" << err;
        }
        ComPtr<ICoreWebView2_3> view3;
        if (SUCCEEDED(m_view.As(&view3)) && view3) {
            HRESULT hr = view3->SetVirtualHostNameToFolderMapping(
                L"mdview.local",
                reinterpret_cast<LPCWSTR>(WebAssetDir().toStdWString().c_str()),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
            if (FAILED(hr))
                qWarning() << "[ndd-mdview] 虚拟主机映射失败, hr=0x" << (quint32)hr;
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
            self->logRenderStats();
        });
        self->m_view->ExecuteScript(L"window.mdvSupportsPost ? 'YES' : 'NO'", probe);
        probe->Release();
    });
    HRESULT hr = m_view->add_NavigationCompleted(navHandler, &token);
    navHandler->Release();
    (void)hr;

    // 网页 -> 宿主的滚动/选区通知
    EventRegistrationToken msgToken;
    WebMessageHandler* msgHandler = new WebMessageHandler([self](const QString& s) {
        if (!self)
            return;
        if (s.startsWith(QLatin1Char('L'))) {
            self->onWebScroll(s.mid(1).toInt());
        } else if (s.startsWith(QLatin1Char('R'))) {
            // 页面内容渲染完了 -> 通知外部跑待办的自测
            if (self->m_contentReadyCb)
                self->m_contentReadyCb();
        } else if (s.startsWith(QLatin1Char('S'))) {
            const QStringList p = s.mid(1).split(QLatin1Char(','));
            if (p.size() == 2)
                self->onWebSelection(p.at(0).toInt(), p.at(1).toInt());
        }
    });
    m_view->add_WebMessageReceived(msgHandler, &msgToken);
    msgHandler->Release();

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
    updateTitle();

    // 渲染统计（节流到 2 秒一次）：既是诊断信息，也方便确认公式渲染是否生效
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastStatsMs > 2000) {
        m_lastStatsMs = now;
        logRenderStats();
    }

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
    if (!m_symbolsResolved) {
        m_symbolsResolved = true;
        m_textFn = reinterpret_cast<SciTextFn>(ResolveExport(kTextSymbol));
        m_isROFn = reinterpret_cast<SciBoolFn>(ResolveExport(kIsReadOnlySymbol));
        m_sendFn = reinterpret_cast<SciSendFn>(ResolveExport(kSendScintillaSymbol));
        m_firstLineFn = reinterpret_cast<SciIntFn>(ResolveExport(kFirstVisibleLineSymbol));
        m_setFirstLineFn = reinterpret_cast<SciVoidIntFn>(ResolveExport(kSetFirstVisibleLineSymbol));
    }

    // 调试钩子：把编辑区真的设成只读（宿主对只读文档会响铃，这条路径平时碰不到）
    if (m_testRO && !m_testRODone && m_sendFn && m_lastEditor.data()) {
        m_testRODone = true;
        m_sendFn(reinterpret_cast<QsciScintilla*>(m_lastEditor.data()),
                 SCI_SETREADONLY, 1, 0);
        qWarning() << "[ndd-mdview] 测试钩子: 已把编辑区设为只读 (SCI_SETREADONLY=2171)"
                      << (m_testForceHostCall ? "，并强制走宿主回调（旧行为）" : "");
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
    // 候选 2 先算：Qt 对象树里认 className == "ScintillaEditView"
    QWidget* ed = FindCurrentEditor(m_host.data());

    // ⚠️ v1.8：只读文档**不能**调宿主的 getCurEdit 回调。
    //    官方回调落到宿主 CCNotePad::getCurEditView()，它对只读文档会：
    //      ui.statusBar->showMessage("The ReadOnly document does not allow this operation.");
    //      QApplication::beep();          <-- 响铃
    //      return nullptr;
    //    我们每个刷新周期都调一次 ⇒ 只读文档上会周期性地「滴」一声、状态栏反复刷提示。
    //    宿主自己的内置 Markdown 预览正是为此绕开了 getCurEditView()
    //    （源码注释原话：「与 getCurEditView() 相同的取法，但不做只读拦截」）。
    //    所以自己先判只读：只读就直接用树里的指针，不去碰宿主回调；
    //    非只读才走官方回调（保住「当前标签页」语义）。
    bool edReadOnly = false;
    if (ed != nullptr && m_isROFn != nullptr)
        SafeSciBool(m_isROFn, reinterpret_cast<QsciScintilla*>(ed), &edReadOnly);

    // 候选 1：宿主给的 getCurEdit 仿函数（只读时跳过，见上）
    QsciScintilla* fromHost = nullptr;
    if ((!edReadOnly || m_testForceHostCall) && m_getCurEdit)
        fromHost = m_getCurEdit(m_host.data());

    if (edReadOnly && !m_roSkipLogged) {
        m_roSkipLogged = true;
        qWarning() << "[ndd-mdview] 当前文档只读：已跳过宿主 getCurEdit 回调"
                      "（否则宿主会响铃并往状态栏刷提示）";
    }

    // 事件驱动的信号要挂在「当前」编辑区上
    if (ed != m_lastEditor.data()) {
        m_lastEditor = ed;
        m_roSkipLogged = false;      // 换文档后允许再提示一次
        connectEditor(ed);
        setupScrollSync(ed);
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
                if (!m_routeLogged) { m_routeLogged = true; qWarning() << "[ndd-mdview] 取文路径 = text()（宿主仿函数指针）"; }
                return true;
            }
        }
        if (ed) {
            memset(slot, 0, sizeof(slot));
            if (CallTextRaw(m_textFn, ed, slot)) {
                TakeTextFromSlot(slot, &out);
                if (!m_routeLogged) { m_routeLogged = true; qWarning() << "[ndd-mdview] 取文路径 = text()（Qt 对象树指针）"; }
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
                    if (!m_routeLogged) { m_routeLogged = true; qWarning() << "[ndd-mdview] 取文路径 = SCI 消息"; }
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

void MdViewPanel::testScrollSync(int line)
{
    qWarning() << "[ndd-mdview] 同步测试: 初始 firstLine=" << editorFirstVisibleLine()
               << " lineCount=" << editorLineCount();
    scrollEditorToLine(line);
    QTimer::singleShot(800, this, [this, line]() {
        qWarning() << "[ndd-mdview] 同步测试: 滚动到第" << line << "行后 firstLine="
                   << editorFirstVisibleLine();
        ExecScriptHandler* h = new ExecScriptHandler(
            [](HRESULT code, LPCWSTR result) {
                if (FAILED(code) || result == nullptr) {
                    qWarning() << "[ndd-mdview] 同步测试: 问不到网页";
                    return;
                }
                qWarning() << "[ndd-mdview] 同步测试: 网页侧 =" << QString::fromWCharArray(result);
            });
        m_view->ExecuteScript(
            L"(function(){"
            L"var els=document.querySelectorAll('#md [data-line]');"
            L"return 'dataLine='+els.length"
            L"+' scrollY='+Math.round(window.pageYOffset||0)"
            L"+' curLine='+(window.mdCurrentLine?window.mdCurrentLine():'no-fn');})()",
            h);
        h->Release();
    });
}

void MdViewPanel::testSelectionSync(int l1, int l2)
{
    if (m_sendFn == nullptr || m_scrollEditor.isNull())
        return;
    int gs = -1, ge = -1;
    if (!SciSelectLines(m_sendFn, reinterpret_cast<QsciScintilla*>(m_scrollEditor.data()), l1, l2,
                        &gs, &ge)) {
        qWarning() << "[ndd-mdview] 选区自测: 选中失败（回读不符 gs=" << gs << " ge=" << ge << "）";
        return;
    }
    qWarning() << "[ndd-mdview] 选区自测: 已在编辑区选中第" << l1 << ".." << l2
               << "行（位置 " << gs << ".." << ge << "，回读确认）"
               << "  |  编辑区视口首行=" << editorFirstVisibleLine()
               << " 总行数=" << editorLineCount();
}

// 自测：反向（预览 -> 编辑区）。
// 在预览里选中第 blockIndex 个块，浏览器的 selectionchange 会触发页面的
// selectionchange 监听，页面按协议 postMessage('S'+起+','+止) 给宿主，
// 宿主再把编辑区选到对应行 —— 由 onWebSelection 打印回读结果。
void MdViewPanel::testWebSelectionSync(int blockIndex)
{
    if (!m_view)
        return;
    const QString js = QStringLiteral(
        "(function(){"
        " var els = document.querySelectorAll('#md [data-line]');"
        " if (!els || els.length <= %1) { return 'no-block(total=' + els.length + ')'; }"
        " var el = els[%1];"
        " var r = document.createRange();"
        " r.selectNodeContents(el);"
        " var s = window.getSelection();"
        " s.removeAllRanges(); s.addRange(r);"
        " return 'picked data-line=' + el.getAttribute('data-line')"
        " + '..' + (el.getAttribute('data-line2') || el.getAttribute('data-line'))"
        " + ' text=' + (s.toString() || '').replace(/\\s+/g, ' ').slice(0, 24);"
        "})()").arg(blockIndex);
    ExecScriptHandler* h = new ExecScriptHandler(
        [](HRESULT code, LPCWSTR result) {
            if (FAILED(code) || result == nullptr) {
                qWarning() << "[ndd-mdview] 网页选区自测: 取回失败";
                return;
            }
            qWarning() << "[ndd-mdview] 网页选区自测: 已在预览里选中 ->"
                       << QString::fromWCharArray(result);
        });
    m_view->ExecuteScript(reinterpret_cast<LPCWSTR>(js.utf16()), h);
    h->Release();
}

// 自测：核对「第 N 行」在编辑区和预览里是不是同一段内容。
// 行号映射一旦错位，「选区同步」就会把左边的第 N 行和右边的另一段配成一对 ——
// 用户看到的就是「左右对不上」。这里把两边第 N 行的文本都打出来对照。
void MdViewPanel::testLineMapping(int line)
{
    if (!m_view)
        return;

    // 编辑区这边：取当前全文，切出第 line 行
    QString editorLine;
    {
        QString all;
        if (readCurrentText(all)) {
            const QStringList ls = all.split(QLatin1Char('\n'));
            if (line >= 1 && line <= ls.size())
                editorLine = ls.at(line - 1).trimmed();
        }
    }

    // 预览那边：找 data-line == line 的块，打印它的文本
    const QString js = QStringLiteral(
        "(function(){"
        " var els = document.querySelectorAll('#md [data-line]');"
        " var hit = [];"
        " for (var i = 0; i < els.length; i++){"
        "   var a = parseInt(els[i].getAttribute('data-line'), 10);"
        "   var b = parseInt(els[i].getAttribute('data-line2'), 10) || a;"
        "   if (a <= %1 && b >= %1) { hit.push(els[i]); }"
        " }"
        " if (!hit.length) { return 'no-block-for-line-%1'; }"
        " var t = (hit[0].innerText || '').replace(/\\s+/g, ' ').trim();"
        " return 'blocks=' + hit.length + ' text=' + t.slice(0, 40);"
        "})()").arg(line);
    ExecScriptHandler* h = new ExecScriptHandler(
        [editorLine](HRESULT code, LPCWSTR result) {
            if (FAILED(code) || result == nullptr) {
                qWarning() << "[ndd-mdview] 行号映射: 预览侧取回失败";
                return;
            }
            qWarning() << "[ndd-mdview] 行号映射: 编辑区 =" << editorLine
                       << "  ||  预览 =" << QString::fromWCharArray(result);
        });
    m_view->ExecuteScript(reinterpret_cast<LPCWSTR>(js.utf16()), h);
    h->Release();
}

// 自测：模拟用户在网页里滚动。
// 直接调页面的 mdScrollToLine（不经过宿主的「推给网页」那条路），
// 所以网页报回来的顶行在宿主看来就是「用户自己滚的」——
// 宿主必须把编辑区滚过去（这正是 v1.5 修的那条被误吞的路径）。
void MdViewPanel::testWebScroll(int line)
{
    if (!m_view)
        return;
    const QString js = QStringLiteral("window.mdScrollToLine(%1);").arg(line);
    m_view->ExecuteScript(reinterpret_cast<LPCWSTR>(js.utf16()), nullptr);
    qWarning() << "[ndd-mdview] 网页滚动自测: 已让网页滚到第" << line << "行";
}

void MdViewPanel::logRenderStats()
{
    if (!m_view)
        return;
    ExecScriptHandler* h = new ExecScriptHandler(
        [](HRESULT code, LPCWSTR result) {
            if (FAILED(code) || result == nullptr) {
                qWarning() << "[ndd-mdview] 渲染统计: 取回失败";
                return;
            }
            qWarning() << "[ndd-mdview] 渲染统计:" << QString::fromWCharArray(result);
        });
    m_view->ExecuteScript(
        L"(function(){var el=document.getElementById('md');"
        L"return 'katex='+document.querySelectorAll('.katex').length"
        L"+' err='+document.querySelectorAll('.md-math-error').length"
        L"+' sel='+document.querySelectorAll('.md-sel').length"
        L"+' text='+(el?el.innerText.length:-1)"
        L"+' head=\"'+(el?el.innerText.slice(0,80).replace(/\\s+/g,' '):'')+'\"';})()",
        h);
    h->Release();
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
    // 页面内容渲染完之后要跑的自测（等 'R' 消息再触发）
    std::function<void(MdViewPanel*)> pendingTest;
    bool pendingTestDone = false;

    void onContentReady()
    {
        if (pendingTestDone || !pendingTest)
            return;
        pendingTestDone = true;
        QTimer::singleShot(200, this, [this]() {
            if (this->panel)
                pendingTest(this->panel);
        });
    }

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
            dock = new QDockWidget(QStringLiteral("Markdown 预览"), mw);
            dock->setObjectName(QStringLiteral("ndd_mdview_dock"));
            panel = new MdViewPanel(dock);
            panel->setContentReadyCallback([this]() { onContentReady(); });
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
            panel->setContentReadyCallback([this]() { onContentReady(); });
            panel->setWindowFlags(Qt::Window);
            panel->setWindowTitle(QStringLiteral("Markdown 预览"));
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
        if (panel)
            panel->applySettings(settings);
    }
};

// ===========================================================================
//  宿主 ABI 导出
// ===========================================================================
static QPointer<MdController> g_controller;

extern "C" {

NDD_EXPORT bool NDD_PROC_IDENTIFY(NDD_PROC_DATA* pProcData)
{
    if (pProcData == nullptr)
        return false;

    pProcData->m_strPlugName = QString::fromUtf8(u8"Markdown 预览 (WebView2)");
    pProcData->m_strComment = QString::fromUtf8(u8"在编辑区右侧渲染 Markdown，"
                                                u8"支持公式（KaTeX），"
                                                u8"基于 Microsoft Edge WebView2");
    pProcData->m_version = QString::fromUtf8(u8"v1.8");
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
        });
    }

    QAction* actSyncScroll = menu->addAction(QStringLiteral("滚动同步"));
    actSyncScroll->setCheckable(true);
    actSyncScroll->setChecked(ctl->settings.syncScroll);
    QObject::connect(actSyncScroll, &QAction::triggered, ctl, [ctl, actSyncScroll]() {
        ctl->settings.syncScroll = actSyncScroll->isChecked();
        ctl->applySettings();
    });

    QAction* actSyncSel = menu->addAction(QStringLiteral("选区同步"));
    actSyncSel->setCheckable(true);
    actSyncSel->setChecked(ctl->settings.syncSelection);
    QObject::connect(actSyncSel, &QAction::triggered, ctl, [ctl, actSyncSel]() {
        ctl->settings.syncSelection = actSyncSel->isChecked();
        ctl->applySettings();
    });

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
            QStringLiteral("Markdown 预览 (WebView2)"),
            QStringLiteral(
                "Notepad-- 插件 · v1.8\n\n"
                "在编辑区右侧用 Microsoft Edge WebView2 渲染 Markdown。\n"
                "支持 GFM 表格、任务列表、围栏代码块、引用，"
                "以及 $...$ / $$...$$ 公式（KaTeX）。\n\n"
                "更新方式：默认事件驱动（编辑区一改就更新，120ms 合并），\n"
                "也可在「自动更新间隔」里改成固定轮询，适合超大文件。\n\n"
                "滚动同步、选区同步都可以单独开关。\n\n"
                "取文依赖宿主 qscintilla 导出的 QsciScintilla::text()。\n"
                "找不到时会退化成读磁盘文件（看不到未保存的改动）。\n\n"
                "v1.8：只读文档不再触发宿主的只读拦截（不响铃、不刷状态栏）。"));
    });

    // 环境变量自动打开面板：NDD_MDVIEW_AUTOSHOW=1
    // （调试用；也方便喜欢常驻预览的人开机就开）
    if (qEnvironmentVariableIsSet("NDD_MDVIEW_AUTOSHOW")) {
        QTimer::singleShot(3000, ctl, [ctl]() {
            ctl->ensure();
            if (ctl->dock) {
                ctl->dock->show();
                ctl->dock->raise();
            } else if (ctl->panel) {
                ctl->panel->show();
            }
        });
    }

    // 环境变量自动隐藏面板：NDD_MDVIEW_AUTOHIDE=1（10 秒后），
    // 用来验证「隐藏时释放 WebView2 内存」这条路径
    if (qEnvironmentVariableIsSet("NDD_MDVIEW_AUTOHIDE")) {
        QTimer::singleShot(10000, ctl, [ctl]() {
            if (ctl->dock)
                ctl->dock->hide();
        });
    }

    // 环境变量注入一个按键：NDD_MDVIEW_TESTTYPE=1
    // 用来验证「改字 → 事件驱动 → 预览更新」这条完整链路
    if (qEnvironmentVariableIsSet("NDD_MDVIEW_TESTTYPE")) {
        QTimer::singleShot(6000, ctl, [ctl]() {
            QWidget* ed = FindCurrentEditor(ctl->host.data());
            if (ed == nullptr) {
                qWarning() << "[ndd-mdview] TESTTYPE: 没找到编辑区";
                return;
            }
            qWarning() << "[ndd-mdview] TESTTYPE: 注入按键";
            ed->setFocus();
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier, QStringLiteral("Z"));
            QApplication::sendEvent(ed, &press);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Z, Qt::NoModifier, QStringLiteral("Z"));
            QApplication::sendEvent(ed, &release);
        });
    }

    // 环境变量自测滚动同步：NDD_MDVIEW_TESTSYNC=1
    if (qEnvironmentVariableIsSet("NDD_MDVIEW_TESTSYNC")) {
        QTimer::singleShot(8000, ctl, [ctl]() {
            if (ctl->panel)
                ctl->panel->testScrollSync(40);
        });
    }

    // ⚠️ 自测钩子必须**等页面内容渲染出来**再跑：
    //    页面脚本就位 ≠ 内容渲染完了 —— 块还没有 data-line，
    //    推送/选择都落空，测出来的「没跳」是假的（踩过两轮这个坑）。
    //    现在由页面渲染完后 postMessage('R') 来触发。

    // 环境变量自测选区同步：NDD_MDVIEW_TESTSEL=1
    if (qEnvironmentVariableIsSet("NDD_MDVIEW_TESTSEL")) {
        // NDD_MDVIEW_TESTSEL=5   -> 选第 5..6 行
        // NDD_MDVIEW_TESTSEL=171 -> 选第 171..171 行（用来复现「选区把视口带跑」）
        const int tsel = qEnvironmentVariableIntValue("NDD_MDVIEW_TESTSEL");
        const int l1 = tsel > 0 ? tsel : 5;
        const int l2 = tsel > 0 ? tsel : 6;
        ctl->pendingTest = [l1, l2](MdViewPanel* p) {
            p->testSelectionSync(l1, l2);
            // 选区是 100ms 轮询发现的，等 1.5s 再取一次渲染统计，
            // 这时 sel= 就是「编辑区选区 -> 预览高亮」的落地证据。
            QTimer::singleShot(1500, p, [p]() { p->logRenderStats(); });
        };
    }

    // 环境变量自测选区同步（反向：预览选区 -> 编辑区选中）：NDD_MDVIEW_TESTSELWEB=1
    // 选第 3 个块（"## 2. 块级公式"），编辑区应该选中第 8 行。
    if (qEnvironmentVariableIsSet("NDD_MDVIEW_TESTSELWEB")) {
        ctl->pendingTest = [](MdViewPanel* p) { p->testWebSelectionSync(3); };
    }

    // 环境变量自测「用户滚网页」：NDD_MDVIEW_TESTSCROLL=200
    // 模拟用户自己滚动网页，宿主必须把编辑区也滚过去。
    if (qEnvironmentVariableIsSet("NDD_MDVIEW_TESTSCROLL")) {
        const int tline = qEnvironmentVariableIntValue("NDD_MDVIEW_TESTSCROLL");
        ctl->pendingTest = [tline](MdViewPanel* p) { p->testWebScroll(tline); };
    }

    // 环境变量自测行号映射：NDD_MDVIEW_TESTMAP=1
    // 8 秒后核对第 1/3/5 行在左右两边是不是同一段内容。
    if (qEnvironmentVariableIsSet("NDD_MDVIEW_TESTMAP")) {
        ctl->pendingTest = [](MdViewPanel* p) {
            const int lines[] = { 1, 3, 5 };
            for (int i = 0; i < 3; ++i) {
                QTimer::singleShot(i * 700, p, [p, ln = lines[i]]() {
                    p->testLineMapping(ln);
                });
            }
        };
    }

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
