# docs —— 官方说明书与参考文档

## plugin-dev-manual.md / plugin-dev-manual.docx

**《Ndd 插件编程开发说明》** —— Notepad-- 官方插件开发说明书全文。

| 项 | 值 |
|---|---|
| 出处 | [gitee.com/cxasm/ndd-plugin](https://gitee.com/cxasm/ndd-plugin) 仓库根目录的 `插件编程开发说明.docx` |
| 作者 | Notepad-- 官方（gitee 用户 `cxasm` / 爬山虎） |
| 许可 | GPL-3.0（同该仓库 LICENSE） |
| 适用版本 | 宿主 NDD 1.21 / 2.0（写于 2023-01 ~ 2023-05） |
| 本仓库实测宿主 | 3.9.1 |

- `plugin-dev-manual.docx`：**官方原件**，未做任何改动，只作存档与校对用。
- `plugin-dev-manual.md`：**从上面的 docx 自动转出的 Markdown 全文**（标题样式 → `#` 层级，
  等宽/代码特征段落 → ` ```cpp ` 代码块，表格 → Markdown 表格）。
  转换脚本见 `scratch/docx_to_md.py`。

### 怎么取到它（无需克隆仓库）

```text
# 1) 文件树（免登录）
https://gitee.com/api/v5/repos/cxasm/ndd-plugin/contents/?ref=master

# 2) 原件
https://gitee.com/cxasm/ndd-plugin/raw/master/插件编程开发说明.docx
```

### 怎么用

本插件怎么用这份说明书、哪些结论以手册为准、哪些以宿主导出为准，
写在主 [README](../README.md) 的 **「官方 ABI 版本口径」** 一节里。

一句话原则：**手册给「接口形状」，宿主导出给「实际可用的符号与消息号」；冲突以宿主导出为准。**
