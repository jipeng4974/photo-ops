# C++ 风格与错误处理

## 格式进入构建流程

`.clang-format` 基于 LLVM，行宽 **96**，`SeparateDefinitionBlocks: Always`，
短函数也保留完整函数体布局。当前实测 clang-format 23.1.3，工具可由 brew 的
`llvm` 提供，或用 `CLANG_FORMAT` 指定路径；这不会改变 GCC 编译器选择。

```bash
xmake format                 # 显式修改源码
xmake format-check           # 只检查
xmake                       # library 构建前也执行相同检查，不偷偷改文件
python -m unittest discover -s tests -p 'test_*.py'
```

clang-format 原生不保证连续函数声明／全局变量之间有空行。因此
`scripts/format.py` 在 clang-format 前后之间补充作用域级的声明间隔，再检查
96 列硬上限。函数、全局变量／常量定义与声明之间留一行；class 的函数声明
也分隔，普通成员字段与函数内局部语句不强制逐条空行。

补充规则是一个跳过注释／字符串并跟踪花括号与参数层级的轻量 lexical pass，
不是通用 C++ AST formatter；覆盖当前模块写法，测试包括 aggregate initializer、
namespace、模板相关作用域、原生 contracts、注释与重复格式化的稳定性。
遇到新的复杂声明语法，先补相应例子，再扩展规则，不用宽泛正则直接扫整份源码。
格式之外，公共接口用 `///` 解释单位、所有权、颜色假设、返回值与错误语义；
函数内按逻辑阶段分段，对意图不直观的步骤解释原因，不逐句翻译代码。

## 可恢复错误和调用违约

依据 [To Err is Human（CppCon 2025）](https://github.com/CppCon/CppCon2025/blob/main/Presentations/To_Err_is_Human.pdf)
中 expected / contracts 的区分：

| 情况 | 项目约定 |
| --- | --- |
| 文件不存在／损坏、格式不支持、ICC 失败、输出写失败 | `result<T>` = `std::expected<T, error>` |
| 用户提供无效 strength、空输入、NaN/Inf 图像 | 在公共边界返回 typed error |
| 非法 image shape、view 超过 storage、像素越界、crop 越界 | 原生 C++26 contracts，视为调用方程序错误 |
| 内存耗尽 | 允许 `std::bad_alloc`，不把每次 allocation 包成返回码 |
| OpenEXR 的异常式 API | 只在 EXR adapter 捕获 `Iex::BaseExc` 并转成 result |
| 其他意外异常 | CLI 最外层兜底；不作为常规控制流 |

`photo_ops.error` 提供错误类别、operation、message 与 path。调用方检查 result
后用 `*result`，需要组合时使用 `transform` / `and_then` / `transform_error`；
不依靠 `.value()` 抛异常传播正常失败，不引入 `TRY` 宏或内部 throw/catch 跳板。
I/O 成功不代表图像适合算法；“没有可信的 sunlit region”是成功的无修改结果。

GCC 编译和链接均启用 `-fcontracts`，编译使用
`-fcontract-evaluation-semantic=quick_enforce`，debug/release 都执行违约检查并立即终止；quick_enforce 不调用自定义 violation handler。
本机 GCC 16.2 + named module/template 组合的实测中，部分模板 `pre(...)`
没有终止非法调用；因此 image/view 在函数体或 allocation 前的计数函数内使用
原生 `contract_assert`。这不是用普通 `assert` 模拟 contracts。七个独立子进程
测试检查非法调用必须以 SIGABRT 终止，且不生成 core 文件，防止只相信语法能编译。
这些故意终止的子进程额外设置 `prctl(PR_SET_DUMPABLE, 0)`：仅设
`RLIMIT_CORE=0` 无法阻止 piped systemd-coredump handler 记录事件并触发桌面通知。
dumpability 只在 fork 后的子进程关闭，正常 test runner 的意外崩溃仍可诊断。
release 优化构建还发现 `enforce` 在 imported image templates 上产生
`__tu_has_violation` 未定义链接错误；outline/conservative-IPA flags 未解决。
当前选择标准 contracts 的 `quick_enforce` 语义，在 debug/release 均实测通过违约
终止测试，保留检查、不关闭 contracts，也不降低语言或模块模式。适合的稳定
非模板接口仍使用 `pre` 表述先决条件。升级 GCC 时可重新检查完整 handler 语义。

不把 contracts 当作不可信输入的校验替代：decode adapter 先验证 metadata，
再构造 image；文件或用户错误应返回 result，而不是触发程序违约。
