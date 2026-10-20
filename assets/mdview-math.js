/* mdview-math.js —— 极简的 $...$ / $$...$$ 公式插件（markdown-it）
 *
 * 思路参考 markdown-it-katex（MIT，https://github.com/mermaid-js/markdown-it-katex），
 * 这里做了精简重写：只依赖全局 katex，不引 npm 运行时。
 *
 * 用法：md.use(window.mdviewMath, {throwOnError:false});
 * 公式渲染失败时退回显示原始 LaTeX（不抛异常，不影响其它内容渲染）。
 */
(function (global) {
    'use strict';

    // 判断某个 $ 能不能作为行内公式的开/闭定界符
    function isValidDelim(state, pos) {
        var max = state.posMax;
        var canOpen = true;
        var canClose = true;
        var prevChar = pos > 0 ? state.src.charCodeAt(pos - 1) : -1;
        var nextChar = pos + 1 <= max ? state.src.charCodeAt(pos + 1) : -1;

        // 闭定界符前面不能是空白；后面紧跟数字也不合法（避免 "$5 and $10" 被误判）
        if (prevChar === 0x20 || prevChar === 0x09 ||
            (nextChar >= 0x30 && nextChar <= 0x39)) {
            canClose = false;
        }
        // 开定界符后面不能是空白
        if (nextChar === 0x20 || nextChar === 0x09) {
            canOpen = false;
        }
        return { can_open: canOpen, can_close: canClose };
    }

    // 行内公式 $...$
    function mathInline(state, silent) {
        if (state.src[state.pos] !== '$') { return false; }

        var res = isValidDelim(state, state.pos);
        if (!res.can_open) {
            if (!silent) { state.pending += '$'; }
            state.pos += 1;
            return true;
        }

        // 跳过被反斜杠转义的 $
        var start = state.pos + 1;
        var match = start;
        var pos;
        while ((match = state.src.indexOf('$', match)) !== -1) {
            pos = match - 1;
            while (state.src[pos] === '\\') { pos -= 1; }
            if (((match - pos) % 2) === 1) { break; }   // 反斜杠个数为偶数 → 是真的闭定界符
            match += 1;
        }
        if (match === -1) {                             // 没找到闭定界符
            if (!silent) { state.pending += '$'; }
            state.pos = start;
            return true;
        }
        if (match - start === 0) {                      // 空内容 $$ 不处理
            if (!silent) { state.pending += '$$'; }
            state.pos = start + 1;
            return true;
        }

        res = isValidDelim(state, match);
        if (!res.can_close) {
            if (!silent) { state.pending += '$'; }
            state.pos = start;
            return true;
        }

        if (!silent) {
            var token = state.push('math_inline', 'math', 0);
            token.markup = '$';
            token.content = state.src.slice(start, match);
        }
        state.pos = match + 1;
        return true;
    }

    // 块级公式 $$...$$
    function mathBlock(state, start, end, silent) {
        var pos = state.bMarks[start] + state.tShift[start];
        var max = state.eMarks[start];
        if (pos + 2 > max) { return false; }
        if (state.src.slice(pos, pos + 2) !== '$$') { return false; }

        pos += 2;
        var firstLine = state.src.slice(pos, max);
        var found = false;
        var lastLine = null;
        var lastPos = 0;
        var next = start;

        if (silent) { return true; }

        // 单行写法：$$...$$ 在同一行内闭合
        if (firstLine.trim().slice(-2) === '$$') {
            firstLine = firstLine.trim().slice(0, -2);
            found = true;
        }

        while (!found) {
            next++;
            if (next >= end) { break; }
            pos = state.bMarks[next] + state.tShift[next];
            max = state.eMarks[next];
            if (pos < max && state.tShift[next] < state.blkIndent) { break; }
            if (state.src.slice(pos, max).trim().slice(-2) === '$$') {
                lastPos = state.src.slice(0, max).lastIndexOf('$$');
                lastLine = state.src.slice(pos, lastPos);
                found = true;
            }
        }

        state.line = next + 1;
        var token = state.push('math_block', 'math', 0);
        token.block = true;
        token.content = (firstLine && firstLine.trim() ? firstLine + '\n' : '')
            + state.getLines(start + 1, next, state.tShift[start], true)
            + (lastLine && lastLine.trim() ? lastLine : '');
        token.map = [start, state.line];
        token.markup = '$$';
        return true;
    }

    function renderInline(latex, options) {
        options.displayMode = false;
        try {
            return global.katex.renderToString(latex, options);
        } catch (e) {
            return '<code class="md-math-error">' + latex + '</code>';
        }
    }

    function renderBlock(latex, options) {
        options.displayMode = true;
        try {
            return '<p>' + global.katex.renderToString(latex, options) + '</p>';
        } catch (e) {
            return '<p><code class="md-math-error">' + latex + '</code></p>';
        }
    }

    global.mdviewMath = function (md, options) {
        options = options || {};
        md.inline.ruler.after('escape', 'math_inline', mathInline);
        md.block.ruler.after('blockquote', 'math_block', mathBlock, {
            alt: ['paragraph', 'reference', 'blockquote', 'list']
        });
        md.renderer.rules.math_inline = function (tokens, idx) {
            return renderInline(tokens[idx].content, options);
        };
        md.renderer.rules.math_block = function (tokens, idx) {
            return renderBlock(tokens[idx].content, options) + '\n';
        };
    };
})(window);
