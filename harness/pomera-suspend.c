/* pomera-suspend: deep suspend helper for armv7 where the apm(8) binary is absent.
 * apm -z equivalent = APM_IOC_SUSPEND ioctl on /dev/apmctl. Called by lid-watch after 2h. */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <machine/apmvar.h>

int main(void) {
  int fd = open("/dev/apmctl", O_RDWR);
  if (fd < 0) fd = open("/dev/apm", O_RDWR);
  if (fd < 0) { fprintf(stderr, "open apm: %s\n", strerror(errno)); return 1; }
  if (ioctl(fd, APM_IOC_SUSPEND, (void *)0) < 0) {
    fprintf(stderr, "ioctl APM_IOC_SUSPEND: %s\n", strerror(errno));
    return 2;
  }
  close(fd);
  return 0;
}
