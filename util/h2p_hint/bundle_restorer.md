# 私有 Bundle GCPT restorer 生成/编译入口

`bundle_restorer.py` 只在新目录生成源/构建，原source只读。模式off/discovery/detail/replay；profiling采用用户确认的独立默认3M退休预算，不加Restore gate、padding或强制裁切2M。性能运行的1M warmup+1M measure由原CPU phase detector保持；profiling driver需要另等Started/busy清零及final真实写完成，本工具不把性能2M退出当作采集结束。

```bash
python3 xs-gem5/util/h2p_hint/bundle_restorer.py \
  --source /absolute/read-only/gcpt_restore \
  --output /absolute/private/restorer-detail \
  --mode detail --budget-million 3 --poll-iterations 1000000
```

默认仅输出JSON计划，不建目录；`--generate-source`创建新私有源副本；`--build --toolchain-proof /absolute/proof.json`在ps1/ps2构建。输出存在即拒绝，无force/resume，不使用原树Makefile，不共享可写构建文件。source匹配当前已审查restore.S/lds的SHA，headers逐个记录hash。默认交叉prefix `/home/xuejing/opt/riscv/bin/riscv64-unknown-linux-gnu-`，可显式指定已验收prefix。

工具链proof格式：

```json
{"schema":"bundle-restorer-toolchain/v1","complete":true,"tools":{"/absolute/resolved/gcc":"SHA256","/absolute/resolved/ld":"SHA256","/absolute/resolved/objcopy":"SHA256","/absolute/resolved/objdump":"SHA256","/absolute/resolved/readelf":"SHA256"}}
```

`complete=true`应由迁移/工具owner在全树hash及有限闭包测试后确认，不能仅下载到某个文件就自行填写。ps1本次工具chain全4923-entry hash已一致，aggregate `86ccffa686cbb344df91727d40781aefcdddf24079ce9e0023a412786e77614a`，tiny交叉编译/readelf/各binutils运行exit0。编译是`gcc -c assembler-with-cpp -march=rv64gc -mabi=lp64d -DCHECKPOINT_NO_RVV -fno-pie -mno-relax`及直接ld，**不链接glibc或startup**；默认动态tiny的库可能包含V，因此明确避开该路径，最终检查ELF Tag_RISCV_arch及反汇编无V。

hook位于原建立sp（GPR metadata指针）之后、首条ld x1之前。保存hook入口mstatus到s3、mie到s4、mtvec到s5；清MIE/MPRV、屏蔽mie、设置私有失败trap入口，以M-mode物理访问已reserved control0x27C200000的sentinel。Off执行同一公共控制状态和哨兵流程，不执行任何Hint CSR；其它模式真实写0x5c2=0x2780、0x5c1=0x2782，fence，写LOAD、轮询bounded timeout并检查Valid/L_Error，然后写Discovery/Detail/Enabled。任何错误或同步trap进入自定义0x6B停机，不继续测量；不伪造DMA完成，不访问host表加载接口。

成功路径按顺序恢复mtvec、mie、mstatus，然后原31条GPR load+mret连续执行，恢复此前被用作临时寄存器的s/t/a，sp最终从原header恢复。既有FPR/CSR恢复逻辑及saved PC/mode不改，mepc/satp/PMP未写；mtime原近似设置保持原顺序。动态临时控制状态保存/恢复和MPRV/PMP权限仍须实际运行验证，不声称所有异步中断环境逐位等效。source只支持原U/S checkpoint，原M-mode saved state仍不支持。

输出包括源计划、gcpt.elf、gcpt.bin、逐命令日志/反汇编/readelf输出、`restorer-manifest.json`（`bundle-restorer/v1`，raw/ELF/disasm SHA）。raw由原linker生成，可能含offset1MiB的默认payload；overlay必须仅按offset4声明size读取低code prefix，**不能整raw覆盖checkpoint**。本次声明size=3072B，超过gem5无RVV外部restorer默认0x700B；必须通过新GCPT overlay内嵌，禁用覆盖它的外部restorer参数/环境，不能把本raw直接交给默认截断路径。

manifest验证唯一31ld+mret、size不触及保存区0xECDB0、无V。非Off还报告成功路径从start CSR之后到mret的静态退休数；本次三个模式均41（含检查/控制状态恢复及尾32），启动csrw本身因现有commit顺序不扣新预算。必须用实际退休序列确认，不能把静态41当作动态guest验证。最终Trace/final drain由独立profiling driver负责，不在prelude内等待profiling结束，否则无法执行workload。

本轮ps1产物位于 `/tmp/bundle-restorer-20261104/build-{off,discovery,detail,replay}/`；工具链proof同父目录。审查报告及四个manifest位于 `results/model/smoke-test/bundle-20261103/environment-migration/restorer-validation/`。源生成dry-run和编译成功；没有执行guest、创建新NC checkpoint、触碰迁移目标树、修改模拟器或SPEC。独立NEMU异步Bundle controller支持也未建立，不能将difftest CSR observer当作它。

overlay工具的`--mode`表示container kind，目前无off值；Off restorer可复用同一合法container kind的overlay输入，但不能因此在Off运行启用Hint capability。共同GCPT原始保存字段与同一runtime由外层A/B流程核对，模式专属低prefix/container hash按各自记录。
