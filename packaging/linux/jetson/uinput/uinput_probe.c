/**
 * @file packaging/linux/jetson/uinput/uinput_probe.c
 * @brief Runtime probe for the NVIDIA Jetson uinput DKMS module.
 */

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

/**
 * @brief Write one input event to a uinput device.
 *
 * @param fd Open uinput file descriptor.
 * @param type Linux input event type.
 * @param code Linux input event code.
 * @param value Event value.
 * @return Zero when the complete event was written, otherwise negative one.
 */
static int emit_event(int fd, unsigned short type, unsigned short code, int value) {
  struct input_event event = {0};

  event.type = type;
  event.code = code;
  event.value = value;
  return write(fd, &event, sizeof(event)) == sizeof(event) ? 0 : -1;
}

/**
 * @brief Create, exercise, and destroy a temporary virtual pointer.
 *
 * @return `EXIT_SUCCESS` when the uinput API accepts the probe, otherwise
 * `EXIT_FAILURE`.
 */
int main(void) {
  struct uinput_setup setup = {0};
  char sysname[128] = {0};
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

  if (fd < 0) {
    perror("open /dev/uinput");
    return EXIT_FAILURE;
  }

  if (ioctl(fd, UI_SET_EVBIT, EV_REL) < 0 ||
      ioctl(fd, UI_SET_RELBIT, REL_X) < 0 ||
      ioctl(fd, UI_SET_RELBIT, REL_Y) < 0) {
    perror("configure uinput");
    close(fd);
    return EXIT_FAILURE;
  }

  setup.id.bustype = BUS_USB;
  setup.id.vendor = 0x1209;
  setup.id.product = 0x0001;
  setup.id.version = 1;
  snprintf(setup.name, sizeof(setup.name), "%s", "Sunshine Jetson uinput probe");

  if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
    perror("create uinput device");
    close(fd);
    return EXIT_FAILURE;
  }

  if (ioctl(fd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0 ||
      emit_event(fd, EV_REL, REL_X, 1) < 0 ||
      emit_event(fd, EV_SYN, SYN_REPORT, 0) < 0 ||
      emit_event(fd, EV_REL, REL_X, -1) < 0 ||
      emit_event(fd, EV_SYN, SYN_REPORT, 0) < 0) {
    perror("exercise uinput device");
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return EXIT_FAILURE;
  }

  printf("Created %s and emitted REL_X +1/-1.\n", sysname);
  if (ioctl(fd, UI_DEV_DESTROY) < 0) {
    perror("destroy uinput device");
    close(fd);
    return EXIT_FAILURE;
  }

  close(fd);
  return EXIT_SUCCESS;
}
