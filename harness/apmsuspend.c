/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 4noha
 *
 * apmsuspend.c — minimal scriptable suspend trigger for pomera DM250
 *   (OpenBSD 7.9 / armv7). base には apm(8)/zzz(8) が入っていないため自作。
 *
 * pomera native build (on pomera, installed):
 *   cc -O2 -Wall -o apmsuspend apmsuspend.c
 *
 * 実値の根拠 (/usr/include/arm/apmvar.h -> #include <arm/apmvar.h>):
 *   #define APM_IOC_SUSPEND _IO('A', 2)   // put system into suspend
 *   ('A'=0x41, group 2, no data) => _IO('A',2) == 0x20004102
 *
 * device / open mode:
 *   apm(8) 系は /dev/apmctl を O_RDWR で開いて ioctl する。
 *   /dev/apmctl は root:wheel 0600 相当 (実機 34,8) なので root/doas 必須。
 *   /dev/apm (34,0) は 0644 だが world は O_RDONLY しか開けず suspend ioctl は
 *   EBADF/EPERM になりうるため、まず /dev/apmctl O_RDWR、だめなら /dev/apm
 *   O_RDWR を試し、最後に /dev/apm O_WRONLY を試す。
 *
 * !!! 実行すると即 suspend する。WiFi(bwfm0) が up のまま suspend すると
 *     カーネルが crash し eMMC FS 破損のおそれ。安全ガードとして、引数に
 *     "--go" を渡さない限り ioctl を発行せず dry-run で終了する。
 *
 *   ./apmsuspend            # dry-run: device を open して即 close、suspend しない
 *   doas ./apmsuspend --go  # 本番: APM_IOC_SUSPEND を発行 (= suspend)
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <machine/apmvar.h>	/* -> <arm/apmvar.h>: APM_IOC_SUSPEND */

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
try_open(const char *path, int mode)
{
	int fd = open(path, mode);
	if (fd >= 0)
		fprintf(stderr, "apmsuspend: opened %s (mode=%d) fd=%d\n",
		    path, mode, fd);
	return fd;
}

int
main(int argc, char *argv[])
{
	int fd = -1;
	int go = 0;

	if (argc > 1 && strcmp(argv[1], "--go") == 0)
		go = 1;

	/* APM_IOC_SUSPEND の実値を表示しておく (検証用) */
	fprintf(stderr, "apmsuspend: APM_IOC_SUSPEND = 0x%08lx\n",
	    (unsigned long)APM_IOC_SUSPEND);

	/* 推奨順: /dev/apmctl O_RDWR -> /dev/apm O_RDWR -> /dev/apm O_WRONLY */
	if ((fd = try_open("/dev/apmctl", O_RDWR)) < 0 &&
	    (fd = try_open("/dev/apm", O_RDWR)) < 0 &&
	    (fd = try_open("/dev/apm", O_WRONLY)) < 0)
		err(1, "open apm device (need root/doas; /dev/apmctl is 0600)");

	if (!go) {
		fprintf(stderr, "apmsuspend: DRY-RUN (no --go) -> NOT issuing "
		    "APM_IOC_SUSPEND. device open OK.\n");
		close(fd);
		return 0;
	}

	fprintf(stderr, "apmsuspend: issuing APM_IOC_SUSPEND now...\n");
	if (ioctl(fd, APM_IOC_SUSPEND, 0) < 0) {
		close(fd);
		err(1, "ioctl APM_IOC_SUSPEND");
	}

	/* ここに到達するのは resume 後 (suspend からカーネルが戻したあと) */
	fprintf(stderr, "apmsuspend: returned from APM_IOC_SUSPEND (resumed)\n");
	close(fd);
	return 0;
}
