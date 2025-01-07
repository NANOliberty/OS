#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

#define BSIZE 512
#define MODE_DEFALUT 0
#define MODE_PRINT (1<<0)
#define MODE_COUNT (1<<1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

char buf[BSIZE];

void _error(const char *msg) {
	printf(1, msg);
	printf(1, "ssufs_test failed...\n");
	exit();
}

void _success() {
	printf(1, "ok\n");
}

void rb_test(char *filename, int blocks, int mode, int steps){
	int fd, i, ret = 0;

	printf(1, "open and read file...\n");
	fd = open(filename, O_RDONLY);

	if(mode & MODE_COUNT){
		printf(1, "rb_count system call start...\n");

		if(rb_count(fd) < 0)
			_error("rb_count system call error\n");
		else
			_success();
	}

	if(mode & MODE_PRINT){
		printf(1, "rb_print system call start...\n");

		if(rb_print(fd) < 0)
			_error("rb_count system call error\n");
		else
			_success();
	}

	if (fd < 0)
		_error("File open error\n");

	for (i = 0; i < blocks; i++) {
		ret = read(fd, buf, BSIZE);
		if (ret < 0) break;
		
		ret = lseek(fd, -BSIZE, SEEK_CUR);
		if (ret < 0) break;

		if(i % steps == steps-1){
			ret = lseek(fd, steps * BSIZE, SEEK_CUR);
			if (ret < 0) break;
		}
	}
	if (ret < 0)
		_error("File read error\n");

	if(mode & MODE_COUNT){
		printf(1, "rb_count system call start...\n");

		if(rb_count(fd) < 0)
			_error("rb_count system call error\n");
		else
			_success();
	}

	if(mode & MODE_PRINT){
		printf(1, "rb_print system call start...\n");

		if(rb_print(fd) < 0)
			_error("rb_count system call error\n");
		else
			_success();
	}

	printf(1, "close file descriptor...\n");

	if (close(fd) < 0)
		_error("File close error\n");
	else
		_success();
}

void test(int ntest, int blocks, int mode) {
	char filename[16] = "file";
	int fd, i, ret = 0;

	filename[4] = (ntest % 10) + '0';

	printf(1, "### test%d start\n", ntest);
	printf(1, "create and write %d blocks...\n", blocks);
	fd = open(filename, O_CREATE | O_WRONLY);

	if (fd < 0)
		_error("File open error\n");

	for (i = 0; i < blocks; i++) {
		ret = write(fd, buf, BSIZE);
		if (ret < 0) break;
	}
	if (ret < 0)
		_error("File write error\n");
	else
		_success();

	printf(1, "close file descriptor...\n");
	
	if (close(fd) < 0)
		_error("File close error\n");
	else
		_success();

	if(mode & MODE_COUNT || mode & MODE_PRINT){
		for(i = 0; i < 3; i++){
			rb_test(filename, blocks, mode, 1 + i);
		}	
	}
	else 
		rb_test(filename, blocks, mode, 1);

	printf(1, "unlink %s...\n", filename);

	if (unlink(filename) < 0)
		_error("File unlink error\n");
	else
		_success();

	printf(1, "open %s again...\n", filename);
	fd = open(filename, O_RDONLY);
	
	if (fd < 0) 
		printf(1, "failed\n");
	else
		printf(1, "this statement cannot be runned\n");

	printf(1, "### test%d passed...\n\n", ntest);
}

int main(int argc, char **argv)
{
	for (int i = 0 ; i < BSIZE; i++) {
		buf[i] = BSIZE % 10;
	}

	// test(1, 5, MODE_COUNT | MODE_PRINT);
	// test(2, 500, MODE_COUNT | MODE_PRINT);
	// test(3, 5000, MODE_COUNT | MODE_PRINT);
	// test(4, 50000, MODE_COUNT | MODE_PRINT);	
		
	/* for test1 */
	// test(1, 5, MODE_DEFALUT);
	// test(2, 500, MODE_DEFALUT);
	// test(3, 5000, MODE_DEFALUT);
	// test(4, 50000, MODE_DEFALUT);	

	/* for test2 */
	test(1, 5, MODE_COUNT | MODE_PRINT);
	test(2, 500, MODE_PRINT);
	test(3, 5000, MODE_COUNT);
	// test(4, 50000, MODE_DEFALUT);	
	exit();
}