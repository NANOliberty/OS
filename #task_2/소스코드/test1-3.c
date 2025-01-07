#include "types.h"
#include "stat.h"
#include "user.h"

#define DEBUG 1

void run_child_process(int end_time) {
    // 각 프로세스의 종료 시간 설정
    set_proc_info(2, 0, 0, 0, end_time); 

    // 프로세스가 끝날 때까지 무한 루프 실행
    while (1) {
        ;  // CPU 사용 부하를 유지하며 스케줄러에 의해 스케줄링되도록 함
    }
}

int main() {
    printf(1, "start scheduler_test\n");
    int pid1 = fork(); 
    if (pid1 == 0) {
        run_child_process(300);
    }

    int pid2 = fork(); 
    if (pid2 == 0) {
        run_child_process(300);
    }

    int pid3 = fork();
    if (pid3 == 0) {
        run_child_process(300); 
    }

    // 부모 프로세스가 모든 자식 프로세스가 종료될 때까지 대기
    for (int i = 0; i < 3; i++) {
        wait();
    }

    printf(1, "end of scheduler_test\n");
    exit();
}