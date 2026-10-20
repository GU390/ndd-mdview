# ndd-mdview

给 [Notepad--](https://gitee.com/cxasm/notepad--) 做的插件：**用 Microsoft Edge WebView2
在编辑器右侧实时渲染 Markdown**，替代内置那套「独立窗口 + Qt `QTextEdit::setMarkdown()`」
的预览。

![类型：插件](https://img.shields.io/badge/type-plugin-0969da)

## 特性

- **右侧停靠**，不是独立窗口；和编辑器同时可见、同时滚动。
- **实时更新**：每 250 ms 轮询编辑区，内容变了就重渲染（只在预览可见时轮询）。
- **完整 Markdown 语法**：GFM 表格、任务列表、围栏代码块、引用、链接自动识别、
  标题层级、水平线 —— 全部由 [markdown-it 14.1.0](https://github.com/markdown-it/markdown-it) 渲染。
- **样式干净**：类 GitHub 的浅色排版，中文友好（微软雅黑）。
- **不链接 qscint**：取文本只借宿主导出的一个符号，不引入 qscintilla 的类布局依赖（见下）。

## 安装

1. 确认已安装 **Microsoft Edge WebView2 Runtime**
   （本机：`C:\Program Files (x86)\Microsoft\EdgeWebView\Application\`）。
2. 把 `ndd-mdview.dll` 复制到 Notepad-- 的 `plugin\` 目录
   （与 `Notepad--.exe` 同级，宿主只扫描 `plugin\*.dll`，不递归）。
3. 重启 Notepad--。
4. 菜单「插件」→「Markdown 实时预览 (WebView2)」，点一下开/关预览面板。

> 插件是**单文件**的：WebView2 的 loader 是静态链接进去的，
> 不需要额外拷 `WebView2Loader.dll`。

## 使用

- 面板在主窗口右侧，可以像普通停靠窗格一样拖动、浮动、调整宽度。
- 切换到别的文件 / 别的标签页，预览会自动跟着当前编辑区走。
- 预览不可见时**不轮询**，不吃 CPU。

## 技术说明

### 文本是怎么取到的

宿主把 qscintilla 静态链进了 `qmyedit_qt5.dll`。踩了三轮才对：

**第 1 轮：Win32 的 SCI 消息 —— 不成立。**
打算枚举主窗口的子孙窗口，找 Scintilla 原生窗口发 `SCI_GETTEXT`。
实测枚举主窗口全部子孙，只有 `Qt5152QWindowIcon` 和 WebView2 的 `Chrome_*`，
**根本不存在 `Scintilla` 原生窗口** —— QScintilla 的 Scintilla 那一侧是个
QWidget（`ScintillaQt`），`SCI_*` 走的是 `ScintillaBase::SendMessage` 虚调用，
不是 Win32 消息，发不出去。

**第 2 轮：调宿主导出的 `QsciScintilla::text()` —— 找到函数了，却崩了宿主。**
`qmyedit_qt5.dll` 的导出表里带着 qscintilla 的全部符号（实测 2403 项），
其中就有：

```
?text@QsciScintilla@@QEBA?AVQString@@XZ     ← QString QsciScintilla::text() const
```

宿主自己的 Markdown 预览（`CCNotePad::slot_markdownPreview`）用的也正是
`pEditView->text()`。所以第一版就照这个思路做：只声明一个同名同签名的最小类，
`GetProcAddress` 拿地址后调用。

结果一调用就 **0xc0000005 崩在 `qmyedit_qt5.dll` 里**：

```
错误模块名称: qmyedit_qt5.dll    异常代码: 0xc0000005    错误偏移量: 0x1c3d7d
```

把偏移映射回导出符号，落在 `QsciScintillaBase::SendScintilla` 入口 +0xd
（第一次解引用 `this`）。再 dump 那个导出函数的机器码核对寄存器：

```
text():          mov r14, rdx   ← this = RDX      mov rbp, rcx  ← sret = RCX
SendScintilla(): mov rcx, [rcx+30h]                ← 第一条就解引用 this
```

寄存器约定和本插件写的一致 ⇒ **不是调用约定问题，是传进去的 `this` 是垃圾**。
而这个 `this` 来自宿主 ABI 里的 `std::function<QsciScintilla*(QWidget*)>` 仿函数
（`getCurEdit`）。跨模块的 `std::function` 到底发生了什么不好深究，
**不再依赖它**。

**第 3 轮（现在用的）：用 Qt 自己的对象树定位编辑区。**

宿主主窗口 → `findChildren<QWidget*>()` → 用 `metaObject()->className()`
认 `"ScintillaEditView"` → 有焦点的优先，否则取可见里最大的那个。
这条路上全是 Qt 自己的 API（`metaObject()` 是虚函数，vtable 在宿主 EXE 里，
跨模块调用完全安全）。

关键前提是继承链：

```
ScintillaEditView : QsciScintilla : QsciScintillaBase
                  : QAbstractScrollArea : QFrame : QWidget : QObject
```

**全是单继承 ⇒ 所有基类子对象偏移都是 0 ⇒ `QWidget*` 就是 `QsciScintilla*`。**
所以拿 QWidget* 直接当 QsciScintilla* 传给 `text()` 是对的。

插件里只声明一个**同名同签名的最小类**：

```cpp
class QsciScintilla { public: QString text() const; };
typedef QString (*SciTextFn)(QsciScintilla* self);
```

用 `GetProcAddress` 拿真实函数地址后调用。`text()` 是**非虚**成员函数，
调用时编译器只把 `this` 原样传下去，不需要知道真实类的布局 ——
所以**不需要 `qsciscintilla.h`，也不链接 qscintilla**，
宿主升级改了 `QsciScintilla` 的成员布局也不会崩。

另外在调用外面裹了一层 `__try/__except`：万一指针不对，只跳过这一轮，
不会把宿主带崩（上一版就是崩在宿主里的）。

代价是：得靠 `GetProcAddress` 认符号名，宿主哪天把 `text()` 改名就得跟着改。
找不到符号时插件会照常加载，只是预览没有内容（停靠窗格标题会写明原因）。

切换标签页后编辑区对象会换，所以每次轮询都重新找一次当前编辑区。

### 文本怎么传给网页

用 base64：编辑区文本先转 UTF-8，再 base64，注入
`window.mdUpdate('...')`。base64 只含 `A-Za-z0-9+/=`，
不用跟 JS 字符串转义规则较劲。JS 侧 `atob` + `TextDecoder('utf-8')` 解回来。

### 面板怎么嵌

`QDockWidget` 挂到 `Qt::RightDockWidgetArea`。宿主主窗口是
`CCNotePad : public QMainWindow`，用 `qobject_cast<QMainWindow*>` 判定；
不是 QMainWindow 时退化成独立顶层窗口。

WebView2 的父句柄是面板自己的 HWND（`WA_NativeWindow` + `winId()`）。
注意顺序：必须先 `dock->setWidget(panel)` 再取 `winId()`，
否则 Qt 会重建原生窗口，WebView2 的父句柄就失效了。
另外停靠位置变化时 Qt 也可能重建原生窗口，`showEvent` 里做了检测并重建控制器。

### 静态 CRT

插件用 `/MT` 静态链接 CRT。宿主目录里只有 14.32 的 `msvcp140.dll`，
云端 VS2022 编出的 `/MD` 会要求更新的运行库，装进宿主会报「找不到入口点」。

## 从源码构建

需要：CMake ≥ 3.20、Qt 5.15.2 MSVC x64（`win64_msvc2019_64`）、
Microsoft.Web.WebView2 1.0.2903.40。

```powershell
# 1) 取 WebView2 SDK（头 + 静态 loader）
#    解压后应得到：
#      3rdparty\webview2\include\WebView2.h
#      3rdparty\webview2\x64\WebView2LoaderStatic.lib
#
# 2) 装 Qt
python -m pip install aqtinstall
python -m aqt install-qt windows desktop 5.15.2 win64_msvc2019_64 -m qtbase --outputdir C:\Qt

# 3) 编
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:\Qt\5.15.2\msvc2019_64
cmake --build build --config Release
# 产物：build\bin\Release\ndd-mdview.dll
```

`.github/workflows/build.yml` 就是把这套流程搬到 `windows-2022` runner 上跑。

## 已知限制

- markdown-it 关闭了原始 HTML（`html:false`），避免 md 里的裸 HTML 破坏预览布局。
- 只渲染**当前**编辑区，不做多标签分屏。
- 宿主是便携版（`NO_PLUGIN` 宏已定义）时才支持插件；官方的「便携版不支持插件」
  提示对应的是另一个构建分支。

## 许可

markdown-it 采用 MIT 协议，版权归其作者所有。
本插件代码随本仓库分发。
