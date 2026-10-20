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
- **不崩宿主**：取文调用外面裹了 SEH，任何意外都只跳过这一轮，不影响宿主。

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

宿主把 qscintilla 静态链进了 `qmyedit_qt5.dll`。踩了四轮才对，过程值得记下来：

**第 1 轮：Win32 的 SCI 消息 —— 不成立。**
打算枚举主窗口的子孙窗口，找 Scintilla 原生窗口发 `SCI_GETTEXT`。
实测枚举主窗口全部子孙，只有 `Qt5152QWindowIcon` 和 WebView2 的 `Chrome_*`，
**根本不存在 `Scintilla` 原生窗口** —— QScintilla 的 Scintilla 那一侧是个
QWidget（`ScintillaQt`），`SCI_*` 走的是 `ScintillaBase::SendMessage` 虚调用，
不是 Win32 消息，发不出去。

**第 2 轮：调宿主导出的 `QsciScintilla::text()` —— 崩宿主。**
`qmyedit_qt5.dll` 的导出表里带着 qscintilla 的全部符号（实测 2403 项），
其中就有：

```
?text@QsciScintilla@@QEBA?AVQString@@XZ     ← QString QsciScintilla::text() const
```

宿主自己的 Markdown 预览（`CCNotePad::slot_markdownPreview`）用的也正是
`pEditView->text()`。所以只声明一个同名同签名的最小类，
`GetProcAddress` 拿地址后调用 —— 结果两次都把宿主带崩了：

| 现象 | 原因 |
|---|---|
| `0xC0000005` 崩在 `qmyedit_qt5.dll` 的 `SendScintilla` 入口 +0xd | 返回槽被当成了 `this`，`mov rcx,[rcx+0x30]` 解引用野指针 |
| `0xC0000409`（BEX64 栈/缓冲区溢出）崩在 Qt5Core 的消息输出路径 | 崩在调用中间，返回槽里留了个半构造的 `QString`，它的析构函数把堆踩坏了 |

第二个坑的修法：**返回槽用裸缓冲（`alignas(8) unsigned char[64]`），不要用
QString 局部变量**。崩了就直接丢弃这块内存，不会有半构造对象等着析构。
另外 `__try/__except` 块里不能放需要回卷的 C++ 对象（MSVC 报 C2712），
所以把「调用」和「拷贝 QString」拆成两个函数。

**第 3 轮：找编辑区。**
宿主 ABI 里的 `getCurEdit` 仿函数能拿到当前编辑区指针；
保险起见还走 Qt 自己的对象树找一遍（`findChildren<QWidget*>()` +
`metaObject()->className() == "ScintillaEditView"`，并优先选祖先里有
`QTabWidget` 的那个 —— 宿主的主编辑区是 `ui.editTabWidget` 的页，
别处也可能藏着 ScintillaEditView）。
实测两个来源返回**同一个地址**，互相印证。

**第 4 轮（就是现在这版）：调用约定搞反了。**
用 capstone 反汇编 `qmyedit_qt5.dll` 里 `text()` 的真实机器码：

```
mov r14, rdx          ; 取入口 RDX
mov rbp, rcx          ; 存入口 RCX
mov edx, 0x887        ; msg = SCI_GETTEXTLENGTH
call SendScintilla    ; RCX 没动 —— 而 SendScintilla 内部是 mov rcx,[rcx+30]
...
mov rax, r14          ; 返回槽地址放 RAX
```

`SendScintilla` 内部把 RCX 当 `this` 解引用，所以 **RCX 必须是 `this`**；
而入口的 **RDX 才是隐藏返回缓冲区**。

**⇒ 成员函数按值返回类类型时，MSVC x64 是 `RCX=this`、`RDX=返回槽`；
自由函数才是 `RCX=返回槽`、`RDX=第一个实参`。**
我一开始套用了自由函数的规则 → 被调方把返回槽当 `this` → 越界 → 崩。

改对之后一切正常：文本取到、实时更新、不崩宿主。

最终取文只依赖两个纯 Win32 事实：宿主 qscintilla 那个 DLL 的导出表里有
`text()`，以及上面这条调用约定。**不链接 qscintilla、不需要它的头文件、
不依赖它的类布局**（`text()` 是非虚成员，调用时 `this` 原样传下去）。

另外还兜了两手：`SendScintilla(SCI_GETLENGTH/SCI_GETTEXT)`（纯 POD 参数与返回），
以及读磁盘文件（编辑区的 `filePath` 动态属性）——
后者看不到未保存的改动，只在前两条都失败时才用，触发时会在日志里明确警告。

切换标签页后编辑区对象会换，所以每次轮询都重新取一次当前编辑区。
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
- 取文依赖宿主 qscintilla 导出表里的 `QsciScintilla::text()`。宿主哪天把这个
  符号改名/去掉，插件会退化成「读磁盘上的文件」—— 能出内容，但看不到未保存的改动，
  停靠窗格标题会提示。日志里会明确写出走了哪条路。
- 超大文件（几十 MB）每次轮询都要整篇取一遍，会有可感知的开销。

## 许可

markdown-it 采用 MIT 协议，版权归其作者所有。
本插件代码随本仓库分发。
