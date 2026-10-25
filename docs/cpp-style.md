# C++ 风格与错误处理

## 格式进入构建流程

v0.2 决定只维护 stock clang-format，不再维护自定义 declaration-spacing pass。
`.clang-format` 基于 LLVM，`ColumnLimit: 96`，`SeparateDefinitionBlocks: Always`，
短函数保留完整函数体。当前实测 clang-format 23.1.3；canonical 来源为 Arch
`clang` 包，也可用 `CLANG_FORMAT` 指定工具路径，不改变 GCC 编译器选择。

```bash
xmake format                 # 显式修改源码
xmake format-check           # 只检查
xmake                       # library 构建前执行相同检查，不偷偷改文件
python -m unittest discover -s tests -p 'test_*.py'
```

先前的 lexical pass 只为了在连续声明／全局变量间强制空行，却需要理解
initializer、lambda、template、contracts 和未来语法的作用域；摄影算子的收益不足以
抵消一个非 AST C++ parser 的长期维护成本。因此删除 pass 及其专属测试，放弃
clang-format 不能直接表达的强制逐声明空行规则。可以按逻辑分段，但不自制解析器
来检查它。`scripts/format.py` 仅负责枚举文件、调用一次 clang-format、展示 diff。
96 列遵从 clang-format 的可折行结果；不再用硬长度检查拒绝不可拆的标识符或文本。
formatter 升级若引起样式变化，显式 review 后提交，不自动扩充自定义规则。

格式之外，公共接口用 `///` 解释单位、所有权、颜色假设、返回值与错误语义；
函数内按逻辑阶段分段，对意图不直观的步骤解释原因，不逐句翻译代码。

## 丢弃返回值与 `[[nodiscard]]`

不为 getter、accessor、数学函数、颜色／图像变换或普通返回值函数添加
`[[nodiscard]]`。是否有返回值本身并不构成“必须使用”的语义；不把可能误用的
每一种写法都升级为编译诊断。需要检查的是错误结果等确有处理义务的类型。

义务属于返回类型时优先在类型层表达，不在每个 producer 上重复标注。
`result<T>` 继续直接 alias 到 `std::expected<T, error>`：虽然 alias 声明不能
给底层类型附加 `[[nodiscard]]`，**当前固定 GCC 16/libstdc++ 已经把 expected
主模板与 void specialization 都标为类型级 `[[nodiscard]]`**，经 named module
导出后仍有效。因此既不需要新增 wrapper，也不需要继承 expected，更不需要
重写它的构造、cv/ref-qualified accessors 和 monadic API。

项目中返回 `result<T>` 的函数不再重复标注，普通 `fail()` helper 返回的
`std::unexpected<error>` 也不额外标注。`transform` / `and_then` 等产生的新
expected 仍保留底层类型的诊断。明确且有理由地忽略结果时可写 `(void)expression`；
不要用此方式隐藏正常错误处理。

这依赖已验证的 **libstdc++ 属性**，不是对所有标准库的可移植性承诺。
`tests/test_result_usage.py` 编译真实项目模块，验证丢弃 `result<int>`、
`result<void>` 及 monadic 结果会触发 `-Werror=unused-result`，正常处理／显式
忽略结果和普通图像／数学 API 则可编译。升级 toolchain 时若行为变化，先检查
库的诊断支持，再评估少量边界标注是否必要，不预先引入 result 框架。

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
