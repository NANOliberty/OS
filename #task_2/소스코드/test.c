// 성능 데이터 수집을 위한 xv6 사용자 프로그램 (응답 시간 및 대기 시간 측정)
#include "types.h"
#include "stat.h"
#include "user.h"

#define NUM_PROCESSES 10

void workload() {
  int i;
  for (i = 0; i < 10000000; i++) {
    asm volatile("nop"); // 간단한 반복 작업 (CPU 부하 생성)
  }
}

int main() {
  int pid, start_time, end_time, wait_time;
  int response_times[NUM_PROCESSES];
  int wait_times[NUM_PROCESSES];

  for (int i = 0; i < NUM_PROCESSES; i++) {
    start_time = uptime(); // 프로세스 시작 시간 기록
    pid = fork();

    if (pid < 0) {
      printf(1, "Fork failed\n");
      exit();
    } else if (pid == 0) {
      // 자식 프로세스: 작업 수행 후 종료
      workload();
      exit();
    } else {
      // 부모 프로세스: 자식 프로세스의 응답 시간 및 대기 시간 측정
      wait();
      end_time = uptime();
      response_times[i] = end_time - start_time;
      wait_time = end_time - start_time;
      wait_times[i] = wait_time;
    }
  }

  // 결과 출력
  printf(1, "Response Times:\n");
  for (int i = 0; i < NUM_PROCESSES; i++) {
    printf(1, "Process %d: %d ticks\n", i, response_times[i]);
  }

  printf(1, "\nWaiting Times:\n");
  for (int i = 0; i < NUM_PROCESSES; i++) {
    printf(1, "Process %d: %d ticks\n", i, wait_times[i]);
  }

  exit();
}
