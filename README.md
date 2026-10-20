# ndd-mdview

给 [Notepad--](https://gitee.com/cxasm/notepad--) 做的插件：**用 Microsoft Edge WebView2
在编辑器右侧渲染 Markdown**，替代内置那套「独立窗口 + Qt `QTextEdit::setMarkdown()`」
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
  反过来滚预览，编辑区也会跟着走。**可开关**。
- **选区同步**：在编辑区选中一段文字（比如一个公式），右侧预览里对应的块会高亮，
  **而且预览里也会做出真正对应的选区**（左右选中的文字是真的同一段）；
  反过来在预览里选中内容，编辑区也会选中对应的行并滚过去。**可开关**。
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
4. 菜单「插件」→「Markdown 预览 (WebView2)」，点一下开/关预览面板。

> 插件是**单文件**的：WebView2 的 loader 是静态链接进去的，
> 不需要额外拷 `WebView2Loader.dll`。

## 使用

菜单「插件」→「Markdown 预览 (WebView2)」→

| 菜单项 | 说明 |
|---|---|
| 显示预览面板 | 开/关右侧预览（勾选状态跟着面板显隐走） |
| 立即刷新 | 手动重取一次文本并重渲染 |
| 自动更新间隔 | 实时（事件驱动，默认）/ 500 毫秒 / 1 秒 / 2 秒 / 5 秒 |
| **滚动同步** | 开/关编辑区 ⇄ 预览的滚动同步 |
| **选区同步** | 开/关编辑区 ⇄ 预览的选区高亮同步 |
| 公式渲染 (KaTeX) | 开关 `$...$` / `$$...$$` 公式排版 |
| 隐藏面板时释放内存 | 隐藏面板时把 WebView2 释放掉（省约 120 MB），再次打开自动重建 |
| 重新加载预览页 | 重新加载网页（改了公式设置后会自动重载） |
| 关于 | 版本与说明 |

- 面板在主窗口右侧，可以像普通停靠窗格一样拖动、浮动、调整宽度。
- 切换到别的文件 / 别的标签页，预览会自动跟着当前编辑区走。
- 预览不可见时**不轮询**，不吃 CPU。
- 设置保存在 `%APPDATA%\Notepad--\ndd-mdview.ini`。
- 停靠窗格标题会显示当前状态：`Markdown 预览 | 每 5 秒 | 913 字符`。

### 环境变量（调试用）

| 变量 | 作用 |
|---|---|
| `NDD_MDVIEW_AUTOSHOW=1` | 启动 3 秒后自动打开预览面板 |
| `NDD_MDVIEW_AUTOHIDE=1` | 启动 10 秒后自动隐藏面板（用来验证释放内存那条路径） |
| `NDD_MDVIEW_TESTTYPE=1` | 启动 6 秒后往编辑区注入一个 `Z` 键（用来验证「改字→预览更新」链路，会真的改到文档里） |
| `NDD_MDVIEW_TESTSYNC=1` | 启动 8 秒后自测滚动同步（把编辑区滚到第 40 行，再读回网页当前行号） |
| `NDD_MDVIEW_TESTSEL=1` | 启动 8 秒后自测选区同步（编辑区选中第 5~6 行 → 预览高亮），9.5 秒后补打一次渲染统计，看 `sel=` 是不是 ≥1 |
| `NDD_MDVIEW_TESTSELWEB=1` | 自测选区同步（反向：在预览里选中第 3 个块 → 编辑区选中对应行） |
| `NDD_MDVIEW_TESTSEL=171` | 自测选区同步（正向：选编辑区第 171 行；**行号可指定**） |
| `NDD_MDVIEW_TESTSCROLL=200` | 自测「用户滚网页」：让网页滚到第 200 行，编辑区应该跟着滚过去 |
| `NDD_MDVIEW_TESTMAP=1` | 自测行号映射：核对第 1/3/5 行在左右两边是不是同一段内容 |

> 自测钩子都会**等页面内容渲染完**再跑（页面渲染完会 postMessage('R') 给宿主）——
> 页面脚本就位 ≠ 内容渲染完了，块还没有 `data-line`，
> 推送/选择都落空，测出来的「没跳」是假的（连踩两轮这个坑）。
> 另外测「选区」时要用**独立的文档副本**：宿主会记住每个文件的光标位置，
> 选区行号跟上次一样的话，插件的变更检测会直接跳过推送，测不出东西。

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

> ⚠️ **回声判定必须精确，不能用「一段时间内一律忽略」。**
> 网页滚完会报一次当前顶行 —— 那个行号跟「我们刚推过去的目标行」一致，就是回声，丢掉。
> v1.5 之前用的是「250ms 内的滚动一律忽略」，结果把**用户自己滚网页**也吞了：
> 两边的视口就此错开，而且**再也回不来**（用户看到的「左右不同步」）。
> v1.6 改成按行号判定（目标行 ±3 行以内、2 秒内 ⇒ 回声），用户自己的滚动不再被吞。
> 同样的道理也适用于**选区**方向：预览回传的行号区间是「整块」的范围，
> 跟宿主推过去的行号**包含** ⇒ 是回声，要挡住（判据是内容，不是时间）。

### 选区同步

和滚动同步同一套行号映射：

1. 每个块级元素带 `data-line`（起始行）和 `data-line2`（结束行）。
2. 编辑区的选区起止位置 → 行号区间 → 网页 `window.mdSyncSelection(s,e)`，
   给**相交**的块加 `.md-sel`（黄底），并且在预览里做一次**真正的原生选区**
   （`Range` + `getSelection().addRange()`）。
   ⇒ 左右两边选中的文字是真的对上的，不只是「右边涂了个黄底」。
   编辑区只有一个光标时**不动**预览的原生选区（否则每敲一行预览就跳一下）。
3. 反方向：网页 `selectionchange` → 算出选中内容所在块的行号区间 →
   `postMessage('S'+起+','+止)` → 宿主用 `SCI_SETSEL` 在编辑区选中对应行，
   并把编辑区滚到那一行（不然选中行在视口外，看着像没同步）。

> ⚠️ **回声判定要按「内容」比，不能按「时间窗」。**
> 这条链子是本插件最隐蔽的一个坑：
> ① 编辑区 → 预览：宿主把选区推给网页，网页按**块**做原生选区；
> ② 网页的 `selectionchange` 报回来的是**整块**的行号范围
>    （一个 61 行的代码块 → `7..69`），不是用户选的那几行；
> ③ 宿主若把它当成「用户选了预览」，就会把编辑区选成整块，
>    **并把编辑区滚到该区间的第一行** —— 视口直接跳「一个块」那么多行。
>
> 用户报的现象是「171 行往上一格选，视口立刻跳到 110 左右」，
> 差值 ≈ 61 行 —— **正好是那个块的大小**。
>
> **本轮用一份 61 行代码块的文档做了 A/B 复现**（同一个钩子、同一份文档）：
>
> | 版本 | 在预览里选中那个 61 行大块后，编辑区视口落到 | 选中内容在不在视野里 |
> |---|---|---|
> | v1.6 | **第 6 行**（区间首行，被拽过去的） | 不在（跑到屏幕外） |
> | v1.7 | 第 48 行（最小滚动，光标行 67 刚好在视野底） | 在 |
>
> 反向（编辑区选 → 预览回传整块区间）在 v1.7 的日志里长这样：
>
> ```
> 选区自测: 已在编辑区选中第 10 .. 10 行（位置 243..303，回读确认） | 编辑区视口首行= 0
> 选区同步: 编辑区选区 -> 10 .. 10（有选区），推给网页
> 选区同步: 网页行 7 .. 69  是本插件刚推过去的选区的回声，忽略      ← ★ 精确识别并无视
> 渲染统计: katex=0 err=0 sel=1 text=2030                          ← 预览里 1 个块高亮
> ```
>
> 视口首行 = 0 —— **编辑区一动不动**，而高亮照常落到预览里。
>
> 修法两条：
> - **回声按内容认**：宿主记下推给网页的区间；网页回传的区间**包含**它 ⇒ 是回声 ⇒ 忽略。
>   （用户真去选同一个块也无妨，结果本来一样。）
>   时间窗口两头都不准：回声迟到（页面忙）会被放行 → 跳行；
>   用户在窗口内真选了预览又会被误杀（v1.7 实测踩到过）。
> - **只做最小滚动**：用 `SCI_ENSUREVISIBLE`（把光标最小幅度滚进视野），
>   不用 `setFirstVisibleLine(区间首行)` —— 选区已经在视野里就**完全不动**。
>
> ⚠️ **「选区推送」不许引起网页滚动，否则编辑区会被带着跳一整个视口。**
> 用户报的现象：编辑区在第 171 行往上选一行，视口立刻跳到 110 行左右
> —— 差值 ≈ 一个视口高度（50~70 行）。
> 成因：预览里设了原生选区（或者插件自己 `scrollIntoView`）→ 网页滚动 →
> 网页把新顶行报回来 → 宿主当成「用户滚了网页」→ 把编辑区也滚过去。
> v1.6 的处理：
> ① `mdSyncSelection` **不再自己 scrollIntoView**（滚动位置交给滚动同步保持对齐）；
> ② 推送选区后开一个 400ms 的窗口，这期间网页报回来的滚动一律不回弹编辑区。

> ⚠️ **选区消息和滚动消息必须用各自的抑制窗口。**
> 用户在预览里拖选时，浏览器常常同时发滚动事件（光标被滚进视野），
> `L` 先到把滚动的抑制窗口撑开，随后到的 `S`（选区）就被同一个窗口一起误杀 ——
> 表现就是「右边选了，左边不动」。v1.5 把选区改成独立的 `m_selSuppressUntil`。
>
> ⚠️ **编辑区指针必须是 `QPointer`，不能是裸 `QWidget*`。**
> 100ms 的滚动/选区轮询一直在用它们，而关掉标签页时宿主会销毁 `ScintillaEditView` ——
> 裸指针就变成悬垂指针，`SendScintilla` 会跑到已释放的内存上取 vtable。
> **SEH 拦不住这种 use-after-free**（地址还是可读的，不触发访问违例），
> 崩在 Qt5Core 里，症状就是「莫名其妙崩了」。v1.5 全部改成 `QPointer`。

> ⚠️ **Scintilla 消息号必须用宿主自带那份头文件里的值。**
> Notepad-- 打包的是自己的 qscintilla 分支，编号跟通用 Scintilla 文档对不上。
> 权威来源：`github.com/cxasm/notepad--` → `src/qscint/scintilla/include/Scintilla.h`。
>
> 本轮踩过的坑（v1.4 修复）：之前写的是
> `SCI_SETSEL=2304` / `SCI_GETSELECTIONEND=2144` / `SCI_LINEFROMPOSITION=2146`，
> 在宿主的头文件里这三个号分别是
> `SCI_CHARLEFT`（光标左移一格）/ `SCI_SETSELECTIONEND`（设置！）/ `SCI_SETPRINTMAGNIFICATION`。
> 后果是**两个方向的选区同步都静默失效**：发出去的 `SCI_SETSEL` 实际是「光标左移」，
> 而每 100ms 的选区轮询里那句「读选区终点」实际是「把选区终点改到 0」，
> 把用户自己拉的选区也搅了。
>
> 教训有两条：
> ① 消息发出去 ≠ 宿主执行了。`SCI_SETSEL` 之后必须**回读** `SCI_GETSELECTIONSTART/END`
>    比对，不符就当失败 —— 只看 `return true` 会被「没抛异常」骗过去。
> ② 同理，日志里那句「已选中」不能当证据：**它无条件打印**。
>    v1.3 的「预览→编辑器 已实测通过」就是被这句日志骗的，
>    实际上编辑区里一个字都没选中。真正的证据要来自回读或截图。
>
> 现在 `SCI_SETSEL` 之后会回读并打印 `位置 x..y（回读确认）`；
> 选区同步的落地证据是渲染统计里的 `sel=`（预览里被加 `.md-sel` 的块数）。

### 排版用现成的 github-markdown-css

样式不再手写 —— 直接用 [github-markdown-css](https://github.com/sindresorhus/github-markdown-css)
（MIT），标题/表格/任务列表/引用/代码块的细节都调过，比手写靠谱得多。
本插件只补了一点自己的东西：滚动条样式、选区高亮、公式的横向滚动与中文回退。

### 更新间隔是真生效的

「实时」= 连编辑区的 `textChanged` 信号（改一个字 120 ms 内就推）；
选了固定间隔就**断开信号**，纯按定时器轮询。所以选了 5 秒就是真的 5 秒才更新一次，
标题栏也会显示「每 5 秒」。

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

## 官方 ABI 版本口径

宿主 ABI 有两个来源：**官方说明书**（规定接口长什么样）和**宿主导出表**（规定这个版本
实际有什么）。这两件事分开记，升级时照着下面两张表逐项核对，不用重新逆向。

> 官方说明书全文在本仓库 [`docs/plugin-dev-manual.md`](docs/plugin-dev-manual.md)
> （原件 `docs/plugin-dev-manual.docx`，出自 [gitee.com/cxasm/ndd-plugin](https://gitee.com/cxasm/ndd-plugin)，
> GPL-3.0，Notepad-- 官方）。它写于 2023 年、对标宿主 1.21/2.0。

### A. 依据手册（接口形状）

| 项 | 手册口径 | 本插件实现 | 手册位置 |
|---|---|---|---|
| 插件入口 | `NDD_PROC_MAIN(QWidget* pNotepad, const QString& strFileName, getCurEdit, pluginCallBack, NDD_PROC_DATA*)` | 同名同签名 | §3.1 / §3.2 |
| 插件标识 | `NDD_PROC_IDENTIFY(NDD_PROC_DATA*)`，填 `m_strPlugName` / `m_strComment` / `m_version` / `m_auther` / `m_menuType` | 5 个字段都填 | §3.1 |
| 二级菜单 | `m_menuType = 1`，在 `s_procData.m_rootMenu` 下自行 `addAction` | 一致（挂在「插件 → Markdown 预览 (WebView2)」下） | §3.2 |
| `pProcData` 生命周期 | **必须拷贝一份**，函数返回后外面会释放 | `s_procData = *pProcData;` | §3.1 / §3.2 |
| `getCurEdit` | 宿主传入的仿函数，返回当前 `QsciScintilla*` | 当作「当前编辑区」的首选来源 | §3.1 |
| `pluginCallBack` | 2.0 起支持：插件可发消息给主程序执行命令 | **未使用**（列为可优化项） | 修改记录表 |
| 编译环境 | Qt 5.15.2 + VS2019 / qmake；插件只依赖 qscintilla | 云端 VS2022 + Qt 5.15.2 + CMake；**不链接 qscintilla** | §2.1 |
| 兼容性 | Qt 版本一致 + **QScintilla ABI 一致** | 规避：不链 qscintilla，运行时 `GetProcAddress` 借宿主导出 | §4 |

> ⚠️ **手册里的函数签名已经过时，照抄会 ABI 不匹配。**
> 手册写的是 1.21 时代的写法：`std::function<QsciScintilla*()> getCurEdit`、
> `std::function<bool(int, void*)> pluginCallBack`（没有 `QWidget*` 首参）。
> 宿主源码 `plugin.h` 里的真实 typedef 已经加了首参：
> `std::function<QsciScintilla*(QWidget*)> getCurEdit`、
> `std::function<bool(QWidget*, int, void*)> pluginCallBack`。
> **本插件跟的是宿主源码**（实测与 3.9.1 运行正常）。

### B. 依据宿主导出（实测值，宿主 3.9.1）

| 事实 | 实测方式 | 值 |
|---|---|---|
| `qmyedit_qt5.dll` 导出总数 | 解析 PE 导出表（`scratch/pe_exports.py`） | **2403**（3.8.3 与 3.9.1 一致，未变） |
| 插件依赖的宿主符号 | `GetProcAddress` 逐个探测 | 7 个**全部命中**（清单见下） |
| Scintilla 消息号 | 宿主源码 `src/qscint/scintilla/include/Scintilla.h` | `SCI_GETSELECTIONSTART 2143` / `SCI_GETSELECTIONEND 2145` / `SCI_SETSEL 2160` / `SCI_LINEFROMPOSITION 2166` / `SCI_POSITIONFROMLINE 2167` / `SCI_ENSUREVISIBLE 2232` / `SCI_GETLINECOUNT 2154` / `SCI_GETFIRSTVISIBLELINE 2152` |
| 编辑控件的类继承 | 宿主源码 `ScintillaEditView` | `QsciScintilla : QsciScintillaBase : QAbstractScrollArea : QFrame : QWidget : QObject`——**单继承 ⇒ 各基类偏移全为 0**，所以 `QWidget* == QsciScintilla*` |
| 菜单挂载点 | 宿主源码 `ccnotepad.cpp`（`onPlugFound` / `slot_markdownPreview`） | 一级菜单下的二级项 |
| 标签页销毁语义 | 同上（`removeTab(index); pw->deleteLater();`） | 关标签页**真的会销毁**编辑控件 ⇒ 插件里一律用 `QPointer`，不能用裸指针 |
| 宿主 Qt 版本 | `Qt5Core.dll` 版本资源 | **5.15.2**（与手册推荐版本一致） |

依赖的 7 个宿主符号（3.9.1 实测全部存在）：

```
?isReadOnly@QsciScintilla@@QEBA_NXZ
?isModified@QsciScintilla@@QEBA_NXZ
?getCurrentLineNumber@QsciScintilla@@QEBAHXZ
?SendScintilla@QsciScintillaBase@@QEBAJIKJ@Z
?text@QsciScintilla@@QEBA?AVQString@@XZ
?firstVisibleLine@QsciScintilla@@QEBAHXZ
?setFirstVisibleLine@QsciScintilla@@QEAAXH@Z
```

### C. 接口演进（手册修改记录表）

| 宿主版本 | 官方新增 |
|---|---|
| 1.21 | 首次支持插件 |
| 1.22.2 | 支持插件二级菜单的创建 |
| 2.0 | 插件可发消息给主程序执行命令 |

本插件按 **≥ 2.0** 口径实现（在宿主 3.9.1 上运行验证）。

### 冲突时以谁为准

1. 手册描述的**函数与字段** → 以**手册**为准（不自己发明 ABI）。
2. 具体**函数地址、消息号、宿主行为** → 以**实测的宿主导出表**为准。
3. 两者对不上（宿主改了导出） → 先信宿主导出表；导出真没了才走降级兜底
   （现在就有兜底：`QsciScintilla::text()` 取不到时退化为读磁盘上的文件，日志会写明走了哪条路）。

### 对照官方文档发现的问题与优化点

**① 只读文档上会周期性「滴」一声（v1.8 已修）**

官方回调 `getCurEdit` 在宿主里落到 `CCNotePad::getCurEditView()`，而这个成员函数
**对只读文档会 `QApplication::beep()` + 往状态栏打一行提示，然后返回 `nullptr`**
（宿主源码 `ccnotepad.cpp`）。v1.7 之前本插件每次刷新都调它，所以打开一个**只读**的
.md 时，默认 500 ms 一次刷新就响一次铃、状态栏反复刷提示。

宿主自己的内置 Markdown 预览正是因此绕开了它 —— 源码注释写着「与 `getCurEditView()`
相同的取法，但**不做只读拦截**：只读文档同样应该允许预览内容」。

**v1.8 的修法**：先用 Qt 对象树找到编辑区、用 `isReadOnly()` 自己判只读；
**只读就直接用树里的指针，不去调宿主回调**，非只读才走官方回调（保持「当前标签页」语义）。
只读时插件日志会打一行「当前文档只读：已跳过宿主 getCurEdit 回调」——
看到这行就说明走的是新路径（v1.7 在同样场景下会反复触发宿主的只读拦截）。

**② `pluginCallBack` 用不上（结论：暂不接）**

手册说它「可以回调 NDD 主程序中的功能函数」，但把宿主源码里
`CCNotePad::pluginInvoke()` 的 switch 展开后，实际只有两条命令：

| cmdId | 作用 | 对本插件的价值 |
|---|---|---|
| 1 | 新建一个文件（`data` 可回传新文件路径） | 无 |
| 2 | 设置当前编辑器语言（0=js / 1=json / 2=html） | 无 —— 没有 markdown 语言项 |

所以这条「官方正路」目前对本插件没有可用命令，接了只是多一个依赖。

**③ 跨模块 RTTI：我们的做法是对的**

宿主源码注释明确警告：插件与主程序分属两个模块，跨模块 `dynamic_cast` /
`qobject_cast` 不可靠，插件拿不到 `ui.editTabWidget`、也认不准编辑控件。
所以本插件**不 link qscintilla、不 `dynamic_cast<ScintillaEditView*>`**，
改用 `metaObject()->className()` 字符串比对 + 「单继承 ⇒ 基类偏移全为 0」的事实，
把 `QWidget*` 直接当 `QsciScintilla*` 用。这条已被官方注释背书。

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

## 崩溃诊断（v1.5）

宿主崩过一次（`Qt5Core.dll` / `0xC0000005` / 偏移 `0x1f6af3`）。
把这条指令反汇编出来是：

```asm
mov      rax, qword ptr [r13 + 8]
mov      rsi, qword ptr [rax + 0x40]
test     rsi, rsi
je       ...
lock inc dword ptr [rsi + 4]     ; 崩在这
```

`lock inc` 是**原子引用计数 +1**；而 `rsi` 是从一个（已失效的）对象里取出来的指针。
再看这个函数所在的地址区间，落在 Qt5Core 的
`QObject::destroyed` / `QMetaObject::disconnect` / `QObjectPrivate::disconnectOne`
之间，函数内部调的是 `QBasicMutex::lockInternal` + `QMetaObjectPrivate::signal` ——
**这就是「断开信号槽连接时遍历连接表」的代码**。

结论：**对已经销毁的 QObject 调 `disconnect()`，在已释放的内存上走连接表 → use-after-free。**

插件里正好有这么两处（宿主源码 `ccnotepad.cpp` 确认了关标签页会
`ui.editTabWidget->removeTab(index); pw->deleteLater();`，编辑控件是真的会被销毁的）：

```cpp
if (m_connEditor) m_connEditor->disconnect(this);   // 编辑区可能已被销毁
if (m_connTab)    m_connTab->disconnect(this);
```

v1.5 的修法：**这些指针全部改成 `QPointer<QWidget>`**。
`QPointer` 在 QObject 被销毁时自动置空，于是「对已销毁对象 disconnect」这条路直接消失；
同时也挡住了 100ms 轮询里 `SendScintilla` / `firstVisibleLine()` 打到野指针上。

> use-after-free 的崩溃是**不确定的**：刚释放、还没被复用的内存通常还是「看起来正常」的，
> 所以本地压力测试不一定能复现（本机 v1.4 跑 6 轮开关标签页也没崩）。
> 真正的暴露窗口是「长时间开着 + 内存被反复分配」的长会话 —— 和用户遇到的情况吻合。
>
> 想拿到确凿的栈，可以让 Windows 崩溃时自动转储（**需要管理员权限**，请自己执行）：
>
> ```powershell
> New-Item -Path 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\Notepad--.exe' -Force | Out-Null
> Set-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\Notepad--.exe' -Name DumpType -Value 2
> Set-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\Notepad--.exe' -Name DumpFolder -Value 'C:\Dumps'
> ```
>
> 下次崩了会在 `C:\Dumps` 留下 `.dmp`，用 WinDbg / Visual Studio 打开就能看到真正的崩溃栈。
- 超大文件每次更新都要整篇取一遍、并让 markdown-it 整篇重排，会有可感知开销；
  这时把「自动更新间隔」调慢一些。
- 打开面板会拉起 WebView2（6 个进程，120~340 MB）。介意的话打开
  「隐藏面板时释放内存」，隐藏后总内存回到 53 MB。

## 许可

markdown-it 采用 MIT 协议，版权归其作者所有。
本插件代码随本仓库分发。

`docs/` 下的《Ndd 插件编程开发说明》是 Notepad-- 官方文档，GPL-3.0 许可，
出处与转出方式见 [`docs/README.md`](docs/README.md)。
