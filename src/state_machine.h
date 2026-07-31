#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

// 啟動三模式狀態機主迴圈，不會返回。
// 呼叫前必須已完成 cyw43_arch_init()、led_status_init()、storage_init()。
void state_machine_run(void);

#endif // STATE_MACHINE_H
