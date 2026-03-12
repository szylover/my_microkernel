#pragma once

/*
 * cmd.h — shell 命令“应用模块”接口
 *
 * 目标：把每个命令（如 cls）做成独立编译单元，shell 只负责注册表与调用。
 * 这不是用户态程序（仍运行在 ring0、和内核一起链接），但在工程组织上更像“应用”。
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*cmd_fn_t)(int argc, char** argv);

typedef struct {
    const char* name;
    const char* help;
    cmd_fn_t fn;
} cmd_t;

#ifdef __cplusplus
}
#endif
