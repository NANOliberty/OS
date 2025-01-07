#include "types.h"
#include "stat.h"
#include "user.h"

#define DEBUG 1  // 디버그 모드 활성화

int main() {
    printf(1, "start scheduler_test\n");
    int pid = fork();  // 자식 프로세스 생성
    if (pid == 0) {  // 자식 프로세스 코드
        // 자식 프로세스의 정보 설정: 0번 큐에서 시작, 총 500 tick 사용 예정
        set_proc_info(0, 0, 0, 0, 500);

        // 자식 프로세스가 500 tick을 소진할 때까지 실행
        while (1) {
            ;  // 빈 루프(스케줄러가 시간 분배를 처리할 수 있게 함)
        }

    }
    wait();
    printf(1, "end of scheduler_test\n");

    exit();  // 부모 프로세스 종료
}
