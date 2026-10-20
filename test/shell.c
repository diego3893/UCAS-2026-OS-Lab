/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * * * * * *
 *            Copyright (C) 2018 Institute of Computing Technology, CAS
 *               Author : Han Shukai (email : hanshukai@ict.ac.cn)
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * * * * * *
 *                  The shell acts as a task running in user mode.
 *       The main function is to make system calls through the user's output.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * * * * * *
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *  * * * * * * * * * * */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#define SHELL_BEGIN 20
#define SHELL_BUFFER_SIZE 256

// exec命令本身占一个位置，其余最多16个程序参数。
#define SHELL_MAX_TOKENS 17

 /**
  * @brief 按空格或制表符拆分命令，直接修改输入缓冲区
  * 
  * @param line 命令行
  * @param tokens 命令+参数
  * @return int 单词数，失败返回-1
  */
static int split_command(char *line, char **tokens){
    int count = 0;
    char *p = line;

    while(*p){
        while(*p==' ' || *p=='\t'){
            p++;
        }

        if(*p == '\0'){
            break;
        }

        if(count >= SHELL_MAX_TOKENS){
            return -1;
        }

        tokens[count++] = p;

        while(*p && *p!=' ' && *p!='\t'){
            p++;
        }

        if(*p){
            *p++ = '\0';
        }
    }

    return count;
}

/**
 * @brief 将十进制字符串转换为正pid
 * 
 * @param str 字符串
 * @param pid 结果pid
 * @return int 1成功，0失败
 */
static int parse_pid(const char *str, pid_t *pid){
    int value = 0;

    if(*str == '\0'){
        return 0;
    }

    while(*str){
        if(*str<'0' || *str>'9'){
            return 0;
        }

        int digit = *str-'0';

        if(value > (2147483647-digit)/10){
            return 0;
        }

        value = value*10+digit;
        str++;
    }

    if(value <= 0){
        return 0;
    }

    *pid = value;
    return 1;
}

/**
 * @brief 解析并执行一条完整命令
 * 
 * @param line 命令行
 */
static void execute_command(char *line){
    char *tokens[SHELL_MAX_TOKENS];
    int length = strlen(line);

    // 去掉末尾空白，再判断是否后台运行。
    while(length>0 && (line[length-1]==' ' || line[length-1]=='\t')){
        line[--length] = '\0';
    }

    int background = length>0 && line[length-1]=='&';

    if(background){
        line[--length] = '\0';
    }

    for(int i=0; i<length; ++i){
        if(line[i] == '&'){
            printf("Invalid use of &.\n");
            return;
        }
    }

    int count = split_command(line, tokens);

    if(count < 0){
        printf("Too many arguments.\n");
        return;
    }

    if(count == 0){
        if(background){
            printf("Missing command.\n");
        }
        return;
    }

    if(background && strcmp(tokens[0], "exec")!=0){
        printf("& is only supported for exec.\n");
        return;
    }

    if(strcmp(tokens[0], "ps") == 0){
        if(count != 1){
            printf("Usage: ps\n");
            return;
        }

        sys_ps();
        return;
    }

    if(strcmp(tokens[0], "clear") == 0){
        if(count != 1){
            printf("Usage: clear\n");
            return;
        }

        sys_clear();
        sys_move_cursor(0, SHELL_BEGIN);
        printf("------------------- COMMAND -------------------\n");
        return;
    }

    if(strcmp(tokens[0], "exec") == 0){
        if(count < 2){
            printf("Usage: exec name [args...] [&]\n");
            return;
        }

        pid_t pid = sys_exec(tokens[1], count-1, &tokens[1]);

        if(pid == 0){
            printf("exec failed.\n");
            return;
        }

        if(background){
            printf("Started pid=%d\n", pid);
        }else{
            sys_waitpid(pid);
        }

        return;
    }

    if(strcmp(tokens[0], "kill") == 0){
        pid_t pid;

        if(count!=2 || !parse_pid(tokens[1], &pid)){
            printf("Usage: kill positive_pid\n");
            return;
        }

        if(sys_kill(pid)){
            printf("Killed pid=%d\n", pid);
        }else{
            printf("No such process.\n");
        }

        return;
    }

    printf("unknown command\n");
}

int main(void){
    char line[SHELL_BUFFER_SIZE];
    int length = 0;
    int too_long = 0;
    int skip_lf = 0;

    sys_move_cursor(0, SHELL_BEGIN);
    printf("------------------- COMMAND -------------------\n");
    printf("> root@UCAS_OS: ");

    while(1){
        // TODO [P3-task1]: call syscall to read UART port
        int ch = sys_getchar();

        if(ch == -1){
            sys_yield();
            continue;
        }

        // TODO [P3-task1]: parse input
        // note: backspace maybe 8('\b') or 127(delete)

        // 将终端发送的\r\n作为一次回车处理。
        if(skip_lf && ch=='\n'){
            skip_lf = 0;
            continue;
        }

        skip_lf = 0;

        if(ch=='\r' || ch=='\n'){
            skip_lf = ch=='\r';

            line[length] = '\0';
            printf("\n");

            // TODO [P3-task1]: ps, exec, kill, clear
            if(too_long){
                printf("Command too long.\n");
            }else{
                execute_command(line);
            }

            length = 0;
            too_long = 0;

            printf("> root@UCAS_OS: ");
            continue;
        }

        if(ch=='\b' || ch==127){
            if(length > 0){
                length--;
                printf("\b");
            }
            continue;
        }

        // 输入制表符时按一个空格处理。
        if(ch == '\t'){
            ch = ' ';
        }

        if(ch>=32 && ch<=126){
            if(length < SHELL_BUFFER_SIZE-1){
                line[length++] = ch;
                printf("%c", ch);
            }else{
                too_long = 1;
            }
        }

        /************************************************************/
        /* Do not touch this comment. Reserved for future projects. */
        /************************************************************/
    }

    return 0;
}