# OTrader 界面设计规范（纯 UI）

仅描述视觉与交互规范：色彩、字体、间距、布局、组件样式。不涉及实现与代码结构。

---

## 1. 主题与层级

**整体风格**：深色、石墨灰、机构交易台风格。无渐变、无装饰色。

### 1.1 背景与表面

| 层级 | 变量 | 色值 | 用途 |
|------|------|------|------|
| 应用底 | `--background` | #06070b | 整页背景 |
| 前景/正文 | `--foreground` | #e5e7eb | 默认文字 |
| 层 1 | `--surface-subtle` | #101218 | 区块、侧栏、日志区、输入框底 |
| 层 2 | `--surface-raised` | #141720 | 悬停、选中、焦点 |

### 1.2 边框

- **常规**：`--border-subtle` #1f242f，1px。用于面板轮廓、表头下边线、输入框、分割线。
- **强调**：`--border-strong` #272f3d，1px。用于焦点、当前导航项左侧竖条、悬停按钮。

### 1.3 文字颜色

- `--text-primary` #e5e7eb：主文字、标题、输入框文字。
- `--text-muted` #6b7280：次要信息、表头、标签、区块小标题。
- `--text-soft` #4b5563：更弱对比（未选导航、占位、日志默认行）。

### 1.4 状态色（仅用于状态与盈亏）

- `--state-success` #22c55e：盈利、运行中、正常、连接成功。
- `--state-error` #ef4444：亏损、错误、已停止、连接失败。
- `--state-warning` #f59e0b：警告、待处理、未连接等中间态。

其余 UI 保持灰度，不滥用彩色。

---

## 2. 文字

### 2.1 字体

- **正文/UI**：Geist Sans。
- **数字/数据/日志**：Geist Mono（等宽），用于金额、数量、时间、日志行。

### 2.2 字号与行高

| 名称 | 字号 | 行高 | 用途 |
|------|------|------|------|
| 页面标题 | 20px（或 18px text-lg） | 1.3 | 页面顶部主标题 |
| 区块标题 | 16px | 1.35 | 页面内区块标题 |
| 面板标题 | 14px | 1.35 | 模块标题、弹窗标题 |
| 正文 | 13px | 默认 | 默认界面文字 |
| 数据值 | 12px | 默认 | 指标、PnL、表格数字 |
| 标签/次要 | 11px | 默认 | 标签、元信息、表头、按钮文字 |
| 日志 | 11px | 1.25 | 日志区每行 |

### 2.3 字间距与大小写

- **标签/按钮**：`letter-spacing: 0.08em`，全大写（uppercase）。
- **表头 / 面板小标题 / 表单标签**：`letter-spacing: 0.12em`，全大写。
- **页面标题**：`tracking-wide`（略加宽），非必须大写。

### 2.4 数字与数据

- 所有金额、数量、比例：Geist Mono + `tabular-nums`，数字**右对齐**。
- 类名约定：`.numeric`（仅字体与 tabular-nums）、`.numeric-12`（12px）、`.numeric-11`（11px）。

---

## 3. 间距

### 3.1 间距阶梯（4px 基准）

| 名称 | 值 | 典型用途 |
|------|-----|----------|
| space-1 | 4px | 图标与文字间距、紧凑 gap |
| space-2 | 8px | 面板头内边距、表头/单元格水平 padding、小按钮 padding |
| space-3 | 12px | 面板体内边距、页面内容区垂直/水平 padding、区块间垂直间距 |
| space-4 | 16px | 区块内分组、较大留白 |
| space-5 | 24px | 大区块间距 |

整体偏紧凑，信息密度高，避免过大留白。

### 3.2 常用组件间距

- **页面内容区**：上下 12px、左右 12px（py-3 px-3）；标题与内容之间 12px（space-y-3）。
- **页面标题**：下边线与文字间距 8px（pb-2）。
- **侧栏**：左右 12px、上下 16px（px-3 py-4）；区块之间 16px（mb-4）。
- **导航项**：左右 12px、上下 8px（px-3 py-2）；项与项之间 2px（space-y-0.5）。
- **表单行**：标签与控件 gap 8px；行与行 space-y-2 或 space-y-3。
- **筛选栏/工具栏**：水平 gap 12px，垂直与内容 12px（gap-3, space-y-3）。

---

## 4. 整体布局（视觉结构）

### 4.1 桌面

- **侧栏**：固定左侧，全高；宽度 176px（w-44 / 11rem）；右边线 1px border-subtle；背景 surface-subtle；内边距 px-3 py-4。
- **主内容区**：侧栏右侧，占满剩余宽度；顶部 65% 为当前页，底部 35% 为日志区；整体最小高度 0 以正确参与 flex。
- **页面内容**：最大宽度 1200px 水平居中；内边距 py-3 px-3；标题与下方内容 space-y-3。

### 4.2 页面内容区结构

- **标题**：单行 h1，大号（text-lg / 18px）、字重 600（semibold）、tracking-wide、下边线 1px border-subtle、下内边距 8px（pb-2）。
- **正文**：标题下方为一个或多个面板，垂直排列；面板内为表单、表格或说明区块。

### 4.3 小屏

- 侧栏隐藏，由顶部栏 + 汉堡菜单展开导航；内容与日志仍上下分块（65% / 35%）。
- **顶部栏**：sticky，下边线 border-subtle，背景 surface-subtle，px-3 py-2；左侧菜单按钮 36×36px（h-9 w-9）；中间当前页标题居中、muted、text-sm。
- **滑出导航**：宽度 176px（w-44），左边固定，背景 surface-subtle，py-4；链接 px-3 py-2，与侧栏样式一致。

---

## 5. 面板

- **容器**：1px 实线边框（border-subtle），背景透明；无圆角、无阴影。
- **面板头**（可选）：内边距 8px 12px（space-2 / space-3）；底部 1px 细线（border-subtle）。
- **面板体**（可选类）：内边距 12px（space-3）。
- **区块内小标题**：14px、全大写、字间距 0.12em、颜色 text-muted。

实际使用中面板常直接加 p-3 或 p-4（12px/16px）统一内边距。

---

## 6. 表格

- **表格容器**：`border-collapse: collapse`，宽度 100%，字号 12px（font-size-data）。
- **表头行**：底部 1px 实线（border-subtle）。
- **th**：内边距 6px 8px；字号 11px；全大写；字间距 0.12em；颜色 text-muted；左对齐；`white-space: nowrap`。
- **td**：内边距 6px 8px。
- **行状态**：悬停行背景 surface-subtle（`.table-row-hover`）；选中行背景 surface-raised（`.table-row-selected`）。
- **列对齐**：数字列右对齐 + numeric-12；文字列左对齐。
- **PnL**：正数用 state-success，负数用 state-error。
- 不画满格线，仅用表头下边线和行背景区分。

---

## 7. 表单

### 7.1 标签

- 字号 11px；全大写；字间距 0.12em；颜色 text-muted（`.form-label`）。

### 7.2 输入框

- **高度**：28px（可选用 h-7 或 h-8 以与按钮对齐）。
- **内边距**：水平 8px，垂直 0。
- **边框**：1px solid border-subtle。
- **背景**：surface-subtle。
- **文字**：text-primary。
- **焦点**：`outline: none`；边框改为 border-strong。
- 数字输入可叠加 `.numeric-11` 或 `.numeric-12`。

### 7.3 布局

- 标签与控件同一行时 gap 8px；多行表单项之间 space-y-2 或 space-y-3。
- 标签可固定宽度右对齐（如 w-24 text-right）形成控制栏对齐。

### 7.4 分段按钮组（如 All / Order / Trade）

- 高度约 28px（h-7）；左右 padding 8px（px-2）；11px 大写；选中项背景 surface-raised、文字 primary；未选中 background surface-subtle、文字 soft，悬停略提对比度；中间用 1px 竖线分隔。

---

## 8. 按钮

### 8.1 基础

- 边框 1px；背景透明；字号 11px；全大写；字间距 0.08em；颜色 text-primary。
- **高度**：主操作常用 32px（h-8），次要或工具栏 28px（h-7），小控件（如日志 Clear）24px（h-6）。
- **水平内边距**：主按钮约 16px（px-4），次要约 8–12px（px-2 / px-3）。

### 8.2 变体

- **主操作**（`.btn-primary`）：边框与文字同基础，边框色改为 border-strong。
- **危险操作**（`.btn-danger`）：边框与文字为 state-error；不做大面积红底。
- **弱化**（`.btn-ghost`）：边框 border-subtle，文字 text-soft；悬停时边框与背景略强。

### 8.3 状态

- **禁用**：透明度约 0.4–0.5（disabled:opacity-40 / disabled:opacity-50），cursor-not-allowed。
- **悬停**：边框可改为 border-strong，背景 surface-raised，文字 primary。

---

## 9. 导航

### 9.1 侧栏

- **背景**：surface-subtle；右边线 1px border-subtle。
- **品牌区**：顶部，居中，主标题 primary，副标题 muted。
- **状态区**：Live Engine、IBKR、Market Data 等；每行左侧为状态点 + 文字；状态点 10×10px 或 12×12px（h-2.5 w-2.5），圆形（rounded-full），颜色按状态 success/error/warning；右侧可带小按钮（约 20×20px）做连接/断开。
- **主导航**：链接块级、居中文字；内边距 px-3 py-2；字号 text-sm；默认文字 text-soft，左边框透明（border-l-2 border-l-transparent）。
- **当前项**：左边框 2px border-strong，文字 text-primary。
- **悬停**：背景可保持或略为 surface-subtle，文字 primary。

### 9.2 主导航顺序

1. Strategy Manager  
2. Backtest  
3. Orders & Trades  
4. Database  

### 9.3 移动端顶部栏与滑出菜单

- 顶部栏：高度约 40px（py-2）；菜单按钮与标题居中；标题为当前页名称、muted。
- 滑出菜单：与侧栏视觉一致（背景、边框、链接样式、当前项左边框）。

---

## 10. 日志区

- **容器**：顶部分割线 1px border-subtle；背景 surface-subtle；全高 flex 列布局。
- **顶部分隔**：可选 1px 高条（border-subtle）再接标题行。
- **标题行**：左右 12px、上下 4px（px-3 py-1）；11px 大写；左侧标题“System Logs”+ 行数，muted；右侧“Clear”按钮：高度 24px（h-6）、左右 8px（px-2）、11px、边框与 ghost 风格一致。
- **内容区**：可滚动；左右 12px、上下 8px（px-3 py-2）；字体 Geist Mono，11px，行高 1.25（`.log-terminal`）；默认行颜色 text-soft。
- **行着色**：含 “ERROR” → state-error；含 “WARN”/“WARNING” → state-warning；其余保持 text-soft。

---

## 11. 其他组件约定

### 11.1 状态条（如 Backend / Live）

- 条带：下边线 border-subtle，背景 surface-subtle，px-4 py-2，字号 text-xs，颜色 text-soft。
- 状态点：约 8×8px（h-2 w-2），圆形，success/error/warning。
- 与文字间距约 6px（gap-1.5）。

### 11.2 弹窗/模态

- 遮罩：半透明深色（如 bg-black/60），全屏，居中。
- 内容框：最大宽度适中（如 max-w-lg），背景 surface-subtle，边框 border-subtle，内边距 p-4；标题 panel-title 风格；底部按钮区右对齐，gap 8px。

### 11.3 指标卡片（如回测结果）

- 小面板：边框 border-subtle，内边距约 8px（px-2 py-2）；标签 10px 大写 muted，与数值间距约 4px（mb-1）；数值 numeric、加粗、可带 success/error 色。

---

## 12. 设计原则（摘要）

- 无渐变、无阴影、无卡片式浮起；层级靠边框与表面色。
- 颜色仅用于：系统/连接/策略状态，以及 PnL/风险提示。
- 其余依赖：字重与字号层级、边框、间距。
- 新 UI 应复用上述面板、表格、表单、按钮与数字样式，保持紧凑、对齐、偏专业工具感，避免营销式或娱乐化视觉。
