# 3DS 端 C 代码规范

本仓库的 `sysmod/`、`app/` 以及仅由 3DS 构建使用的代码，遵循 Linux kernel
driver 风格。目标是让协议状态机、DMA/缓存操作和并发边界在代码结构上直接可见，
而不是只在注释里可见。

## 基本格式

- 缩进使用 **8 字符 tab**；续行用 tab + 空格对齐。
- 每行不超过 **80 列**；禁止一行多条语句（`a = 1; b = 2;`）。
- 函数、`if/for/while/switch` 使用 K&R 大括号：

```c
static int live_probe(struct live_stream *ls)
{
        int ret;

        ret = live_socket_open(ls);
        if (ret)
                return ret;

        return 0;
}
```

- 指针星号靠近变量名：`struct screen *screen`。
- 一元运算符不留空格；二元运算符两侧留空格；`sizeof(x)` 不加空格。
- 注释使用 `/* ... */`；只在解释“为什么”时写注释，不重复代码。

## 命名与作用域

- 函数、变量、结构体字段使用小写下划线：`live_stream_start()`、
  `screen->observed_addr`。
- 不使用 `typedef` 隐藏指针或结构体；设备状态用 `struct live_stream`，
  不写 `LiveStream`、`Screen` 这类大小写混合的类型名。
- 文件内状态一律 `static`；跨文件接口放头文件，并配 `live_` 前缀。
- 全局可变状态集中到一个 `struct`，通过指针传给函数，禁止散落的
  `static u32 foo, bar, baz;`。
- 布尔值用 `bool` 和 `true/false`；不要用 `int` 兼作布尔。
- 错误码用 `int` 返回 0/负 errno 或 libctru `Result`，入口统一校验参数。

## 函数与复杂度

- 单个函数尽量不超过一屏（约 40–50 行）；超过时拆成有名字的步骤。
- 参数超过 5 个时，改为传结构体指针。
- 早返回处理错误；正常路径保持左侧低缩进。
- 禁止在条件表达式里做赋值或副作用。
- 状态机使用 `enum` + `switch`，每个状态集中处理自己的转换。

## 并发与硬件

- 明确标注锁：`/* Called with ls->lock held. */`。
- 共享字段的读写使用同一把锁或 `__atomic_*`；不要混用。
- DMA/缓存操作前后用注释说明方向与可见性。
- 中断/线程入口只做分发，业务放在可单测的纯函数里。

## 提交与检查

- 每个提交只做一类改动：`style(3ds): ...`、`refactor(3ds): ...`、
  `fix(3ds): ...`。
- 提交前运行：
  - `pwsh -File scripts/check-3ds-style.ps1`
  - `make test`（主机协议测试）
  - `pwsh -File scripts/build-handheld.ps1 -Bash <msys-bash>`
- 不允许为了过格式检查而改变协议字节、时序或线程模型；行为变更必须单独提交。
