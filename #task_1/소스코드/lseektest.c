#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

// lseek에서 사용할 매크로 정의
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int main(int argc, char *argv[]) {
    int fd, offset;
    char buffer[512];

    if (argc != 4) {
        printf(1, "Usage: lseektest <filename> <offset> <string>\n");
        exit();
    }

    // 파일 열기 (읽기/쓰기 모드)
    fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        printf(1, "Error: cannot open file %s\n", argv[1]);
        exit();
    }

    // 파일 처음부터 읽기 (Before 상태 출력)
    if (lseek(fd, 0, SEEK_SET) < 0) {
        printf(1, "Error: lseek failed to reset file offset\n");
        close(fd);
        exit();
    }

    int n = read(fd, buffer, sizeof(buffer) - 1);
    if (n < 0) {
        printf(1, "Error: read failed\n");
        close(fd);
        exit();
    }
    buffer[n] = '\0';
    printf(1, "Before : %s", buffer);

    // 오프셋 이동
    offset = atoi(argv[2]);
    if (lseek(fd, offset, SEEK_SET) < 0) {
        printf(1, "Error: lseek failed\n");
        close(fd);
        exit();
    }

    // 새로운 문자열에 개행 문자 추가하여 쓰기
    int length = strlen(argv[3]);
    argv[3][length] = '\n';  // 입력된 문자열 끝에 개행 문자 추가
    argv[3][length + 1] = '\0';  // 널 문자 추가

    if (write(fd, argv[3], length + 1) < 0) {
        printf(1, "Error: write failed\n");
        close(fd);
        exit();
    }

    // 파일 처음부터 읽기 (After 상태 출력)
    if (lseek(fd, 0, SEEK_SET) < 0) {
        printf(1, "Error: lseek failed to reset file offset\n");
        close(fd);
        exit();
    }

    n = read(fd, buffer, sizeof(buffer) - 1);
    if (n < 0) {
        printf(1, "Error: read failed\n");
        close(fd);
        exit();
    }
    buffer[n] = '\0';
    printf(1, "After : %s", buffer);

    // 파일 닫기
    close(fd);
    exit();
}
