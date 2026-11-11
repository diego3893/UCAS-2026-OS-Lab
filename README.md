# OSLab 起始代码

  这是被仲剑修改过后的起始代码！

#### [2024-08-12]

  修改了 libs/string.c 中的`strncmp()`函数。

  之前在 issues 里提到的 createimage 工具的“严重缺陷”，考虑到同学们看了可能过于疑惑，暂时没敢修改……

  修改了 tiny_libc/printf.c 中的一些函数，使用户 C 库函数`printf()`能正确支持`%d`；修改了 libs/printk.c 中的一些函数，使内核函数`printk()`能正确支持`%d`并且支持空格填充而非只能零填充。

  修复了 drivers/screen.c 中的一些函数可能存在的缓冲区溢出的漏洞。

#### [2024-08-13]

  从分支 24fall-p2-start 合并过来。

  修复了 test/test_project3/sema_producer.c 的打印位置问题，以及 test/test_project3/mbox_client.c 的打印出的计数问题。

  进一步修复了 drivers/screen.c 中的屏幕驱动程序。

  从分支 24fall-p3-start 合并过来。

