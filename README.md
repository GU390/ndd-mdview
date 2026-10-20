# ndd-mdview

给 [Notepad--](https://gitee.com/cxasm/notepad--) 做的插件：**用 Microsoft Edge WebView2
在编辑器右侧实时渲染 Markdown**，替代内置那套「独立窗口 + Qt `QTextEdit::setMarkdown()`」
的预览。

![类型：插件](https://img.shields.io/badge/type-plugin-0969da)

## 特性

- **右侧停靠**，不是独立窗口；和编辑器同时可见、同时滚动。
- **实时更新**：默认**事件驱动** —— 直接监听编辑区的 `textChanged` 信号，
  改一个字 120 ms 内就出现在预览里；空闲时**完全不轮询，CPU 占用 0.00%**。
- **刷新率可调**：菜单里能切成 500 毫秒 / 1 秒 / 2 秒 / 5 秒固定轮询，
  超大文件建议调慢。
- **完整 Markdown 语法**：GFM 表格、任务列表、围栏代码块、引用、链接自动识别、
  标题层级、水平线 —— 全部由 [markdown-it 14.1.0](https://github.com/markdown-it/markdown-it) 渲染。
- **公式渲染**：`$...$` 行内公式和 `$$...$$` 块级公式，用
  [KaTeX 0.16.11](https://katex.org) 排版；公式里夹中文也能正常显示。
- **滚动同步**：拖动编辑区的滚动条，右侧预览跟着滚到同一段内容；
  反过来滚预览，编辑区也会跟着走。
- **省内存**：隐藏面板时可释放 WebView2（省约 120 MB），再次打开自动重建。
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

菜单「插件」→「Markdown 实时预览 (WebView2)」→

| 菜单项 | 说明 |
|---|---|
| 显示预览面板 | 开/关右侧预览（勾选状态跟着面板显隐走） |
| 立即刷新 | 手动重取一次文本并重渲染 |
| 自动更新间隔 | 实时（事件驱动，默认）/ 500 毫秒 / 1 秒 / 2 秒 / 5 秒 |
| 公式渲染 (KaTeX) | 开关 `$...$` / `$$...$$` 公式排版 |
| 隐藏面板时释放内存 | 隐藏面板时把 WebView2 释放掉（省约 120 MB），再次打开自动重建 |
| 重新加载预览页 | 重新加载网页（改了公式设置后会自动重载） |
| 关于 | 版本与说明 |

- 面板在主窗口右侧，可以像普通停靠窗格一样拖动、浮动、调整宽度。
- 切换到别的文件 / 别的标签页，预览会自动跟着当前编辑区走。
- 预览不可见时**不轮询**，不吃 CPU。
- 设置保存在 `%APPDATA%\Notepad--\ndd-mdview.ini`。

### 环境变量（调试用）

| 变量 | 作用 |
|---|---|
| `NDD_MDVIEW_AUTOSHOW=1` | 启动 3 秒后自动打开预览面板 |
| `NDD_MDVIEW_AUTOHIDE=1` | 启动 10 秒后自动隐藏面板（用来验证释放内存那条路径） |
| `NDD_MDVIEW_TESTTYPE=1` | 启动 6 秒后往编辑区注入一个 `Z` 键（用来验证「改字→预览更新」链路，会真的改到文档里） |
| `NDD_MDVIEW_TESTSYNC=1` | 启动 8 秒后自测滚动同步（把编辑区滚到第 40 行，再读回网页当前行号） |

## 性能实测

本机（i3-9100 / 8 GB 单通道 / Win10），面板打开、什么都不动：

| 状态 | Notepad-- 自身 | WebView2 | 合计内存 |
|---|---|---|---|
| 面板打开（实时模式，空闲） | **CPU 0.00%** / 54 MB | 6 进程 / 120~340 MB | 约 175~390 MB |
| 隐藏并释放 WebView2 之后 | **CPU 0.00%** / 53 MB | 0 进程 / 0 MB | **约 53 MB** |

- CPU 能到 0.00% 是因为改成了**事件驱动** —— 旧版每 250 ms 轮询一次取全文，
  即使一个字没改也在不停复制文档。现在靠编辑区的 `textChanged` 信号触发，
  另有 100 ms 一次的看门狗只读一个「首行」整数（不复制文档），所以不涨。
- WebView2 的内存波动很大：刚打开时浏览器缓存还没收缩（最高能到 340 MB），
  闲置几分钟后会自己降到 120 MB 左右。介意的话打开
  「隐藏面板时释放内存」，隐藏后总内存直接回到 53 MB。

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

### 滚动同步怎么做的

两边靠**行号**对齐，不是按比例：

1. markdown-it 的块级 token 带 `token.map`（行号），渲染时给每个块级元素
   加一个 `data-line="N"`（源码行号，1 基）。
2. 编辑区的滚动位置用 qscintilla 导出的两个高层 API 读写：
   ```
   ?firstVisibleLine@QsciScintilla@@QEBAHXZ      ← int  firstVisibleLine() const
   ?setFirstVisibleLine@QsciScintilla@@QEAAXH@Z  ← void setFirstVisibleLine(int)
   ```
   ⚠️ **别去用 `QAbstractScrollArea` 那个 QScrollBar** —— QScintilla 并不用它，
   实测 `setValue()` 改了值但编辑区根本不滚（而且那个滚动条的值会自己乱跳）。
   `SCI_LINESCROLLTO` QScintilla 也没实现，同样无效。
3. 编辑区首行变了 → 调网页的 `window.mdScrollToLine(行号)`，
   找到 `data-line <= 行号` 里最大的那个块，滚到它的顶部。
4. 反方向：网页滚动时用 `window.chrome.webview.postMessage('L'+行号)` 通知宿主，
   宿主再调 `setFirstVisibleLine()` 滚编辑区。
5. 双向都会回声，所以同步时置一个 250 ms 的抑制窗口，避免来回抖。

**粒度是「块」**：编辑区第 40 行如果落在从第 14 行开始的那一段里，
预览会滚到那段的开头 —— 和 Typora / Obsidian 的行为一致。

检测编辑区滚动用 100 ms 轮询一次「首行」（只读一个 int，不是复制整篇文档），
所以开着不动时 CPU 仍然是 0.00%。

### 公式渲染（KaTeX）

- 引擎：[KaTeX 0.16.11](https://katex.org)（MIT），CI 里从 npm 拉，版本钉死。
- 排版插件是自己写的精简版（`assets/mdview-math.js`，思路参考
  markdown-it-katex）：行内 `$...$`、块级 `$$...$$`，并处理了
  「`$5 和 $10` 不该被当成公式」「`\$100` 转义」这些边界。
  公式语法出错时退回显示原始 LaTeX，不影响其它内容渲染。
- 资源怎么送到网页：KaTeX 的 js / css / 20 个 woff2 字体编译进 DLL，
  首次使用时解到 `%TEMP%\ndd-mdview\katex-0.16.11\`，再用 WebView2 的
  **虚拟主机映射**（`https://mdview.local/` → 该目录）暴露给网页。
  这样页面本体很小，字体**按需加载**（用到哪个字形才下载哪个字体文件），
  不会 20 个字体全进内存。

### 更新是怎么触发的

- 默认**事件驱动**：用字符串式 `QObject::connect(editor, SIGNAL(textChanged()), ...)`
  连宿主的编辑区信号 —— 不需要 `QsciScintilla` 的类定义，跨模块也安全。
  信号本身很密（每个按键都发），所以用 120 ms 的单次定时器合并成一次推送。
- 另有一个 1 秒的**看门狗**：编辑区可能是插件加载之后才建好的（那时信号还没连上），
  切换标签页 / 换文件时对象也会变。看门狗只做「重新定位 + 比对文本」，
  文本没变就不推送，所以不会产生 markdown-it 重渲染的开销。
- 也可以在菜单里切成固定间隔轮询（500 ms ~ 5 s），适合超大文件。

### 文本怎么推给网页

优先用 WebView2 的 **原生消息通道**（`PostWebMessageAsString`）：
直接把文本字符串发给网页，省掉 UTF-8 转换、base64 编码、JS 表达式拼装与再解析。

页面加载完后宿主会问一句 `window.mdvSupportsPost`；页面答不上来（比如
`window.chrome.webview` 不可用）就自动退回 **base64 + ExecuteScript**，
功能不受影响，只是多几次拷贝。

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

# 3) 取 KaTeX 资源（公式渲染用，CI 里也是这一步）
npm pack katex@0.16.11
tar -xzf katex-0.16.11.tgz
mkdir -p assets\fonts
copy package\dist\katex.min.js assets\
copy package\dist\katex.min.css assets\
copy package\dist\fonts\*.woff2 assets\fonts\
# 再照着 assets\ 里的文件生成一份 assets\katex.qrc（见 build.yml 的 "Fetch KaTeX assets"）

# 4) 编
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:\Qt\5.15.2\msvc2019_64
cmake --build build --config Release
# 产物：build\bin\Release\ndd-mdview.dll
```

> 不取 KaTeX 也能编（`assets\*.qrc` 是 glob 的，没有 katex.qrc 就不带公式资源），
> 只是公式渲染不可用。

`.github/workflows/build.yml` 就是把这套流程搬到 `windows-2022` runner 上跑。

## 已知限制

- markdown-it 关闭了原始 HTML（`html:false`），避免 md 里的裸 HTML 破坏预览布局。
- 只渲染**当前**编辑区，不做多标签分屏。
- 宿主是便携版（`NO_PLUGIN` 宏已定义）时才支持插件；官方的「便携版不支持插件」
  提示对应的是另一个构建分支。
- 取文依赖宿主 qscintilla 导出表里的 `QsciScintilla::text()`。宿主哪天把这个
  符号改名/去掉，插件会退化成「读磁盘上的文件」—— 能出内容，但看不到未保存的改动，
  停靠窗格标题会提示。日志里会明确写出走了哪条路。
- 公式排版用 KaTeX，只支持 TeX 常见语法；需要 Mermaid 流程图 / 折叠区块之类
  得再接对应的前端库。
- 超大文件每次更新都要整篇取一遍、并让 markdown-it 整篇重排，会有可感知开销；
  这时把「自动更新间隔」调慢一些。
- 打开面板会拉起 WebView2（6 个进程，120~340 MB）。介意的话打开
  「隐藏面板时释放内存」，隐藏后总内存回到 53 MB。

## 许可

markdown-it 采用 MIT 协议，版权归其作者所有。
本插件代码随本仓库分发。
