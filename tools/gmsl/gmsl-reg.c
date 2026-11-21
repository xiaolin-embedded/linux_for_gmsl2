// SPDX-License-Identifier: GPL-2.0
/*
 * gmsl-reg - register access for MAX96724 GMSL2 links
 *
 * MAX96724 forwards I2C transactions for addresses other than its own through
 * the enabled GMSL link.  On the SG2-IMX390C module the visible devices are:
 *
 *   deserializer 0x27
 *   serializer   0x41 + link index (after the driver has probed it)
 *   GW5200 ISP   0x6d (0xDA 8-bit address)
 *   EEPROM       0x51 (0xA2 8-bit address)
 *
 * IMX390 is at 0x21 (0x42 8-bit address), but it sits behind the GW5200 ISP
 * and is not directly visible on the serializer I2C bus.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DES_ADDR		0x27
#define SER_BASE_ADDR		0x40
#define ISP_ADDR		0x6d
#define EEPROM_ADDR		0x51
#define SENSOR_ADDR		0x21
#define MAX_REG_16		0xffff
#define MAX_REG_8		0xff
#define MAX_BYTES		4096

struct gmsl_ctx {
	int fd;
	int bus;
	int link;
	int reg_bytes;
	int retries;
};

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static int parse_u32(const char *s, unsigned int max, unsigned int *out)
{
	char *end;
	unsigned long val;

	errno = 0;
	val = strtoul(s, &end, 0);
	if (errno || end == s || *end || val > max)
		return -1;

	*out = val;
	return 0;
}

static int find_bus(void)
{
	char path[320], name[128];
	struct dirent *de;
	DIR *dir;
	ssize_t n;
	int fd, bus;

	dir = opendir("/sys/bus/i2c/devices");
	if (!dir)
		return -1;

	while ((de = readdir(dir))) {
		if (!isdigit(de->d_name[0]))
			continue;

		snprintf(path, sizeof(path), "/sys/bus/i2c/devices/%s/name",
			 de->d_name);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		n = read(fd, name, sizeof(name) - 1);
		close(fd);
		if (n <= 0)
			continue;
		name[n] = '\0';
		if (!strstr(name, "max96724"))
			continue;

		bus = atoi(de->d_name);
		closedir(dir);
		return bus;
	}

	closedir(dir);
	return -1;
}

static int open_bus(struct gmsl_ctx *ctx)
{
	char path[64];

	if (ctx->bus < 0)
		ctx->bus = find_bus();
	if (ctx->bus < 0)
		die("cannot autodetect the MAX96724 I2C bus; use -b <bus>");

	snprintf(path, sizeof(path), "/dev/i2c-%d", ctx->bus);
	ctx->fd = open(path, O_RDWR | O_CLOEXEC);
	if (ctx->fd < 0)
		die("open %s: %s", path, strerror(errno));

	return 0;
}

static int i2c_xfer(struct gmsl_ctx *ctx, struct i2c_msg *msgs, int nmsgs)
{
	struct i2c_rdwr_ioctl_data xfer = {
		.msgs = msgs,
		.nmsgs = nmsgs,
	};
	int attempt, err = EIO, ret;

	for (attempt = 0; attempt <= ctx->retries; attempt++) {
		ret = ioctl(ctx->fd, I2C_RDWR, &xfer);
		if (ret == nmsgs)
			return 0;

		err = ret < 0 ? errno : EIO;
		if (ret < 0 && err != ENXIO && err != EREMOTEIO && err != EIO)
			return -err;
		if (attempt < ctx->retries)
			usleep(1000);
	}

	return -err;
}

static int i2c_read_byte(struct gmsl_ctx *ctx, int addr, unsigned int reg,
			 uint8_t *val)
{
	uint8_t regbuf[2];
	struct i2c_msg msgs[2];

	if (ctx->reg_bytes == 2) {
		regbuf[0] = reg >> 8;
		regbuf[1] = reg;
	} else {
		regbuf[0] = reg;
	}

	msgs[0].addr = addr;
	msgs[0].flags = 0;
	msgs[0].len = ctx->reg_bytes;
	msgs[0].buf = regbuf;

	msgs[1].addr = addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = 1;
	msgs[1].buf = val;

	return i2c_xfer(ctx, msgs, 2);
}

static int i2c_write_byte(struct gmsl_ctx *ctx, int addr, unsigned int reg,
			  uint8_t val)
{
	uint8_t buf[3];
	struct i2c_msg msg;

	if (ctx->reg_bytes == 2) {
		buf[0] = reg >> 8;
		buf[1] = reg;
		buf[2] = val;
	} else {
		buf[0] = reg;
		buf[1] = val;
	}

	msg.addr = addr;
	msg.flags = 0;
	msg.len = ctx->reg_bytes + 1;
	msg.buf = buf;

	return i2c_xfer(ctx, &msg, 1);
}

static void print_reg(FILE *f, struct gmsl_ctx *ctx, unsigned int reg)
{
	if (ctx->reg_bytes == 2)
		fprintf(f, "0x%04x", reg);
	else
		fprintf(f, "0x%02x", reg);
}

static int resolve_target(struct gmsl_ctx *ctx, const char *name, int *addr)
{
	unsigned int val;

	if (!strcasecmp(name, "des") || !strcasecmp(name, "deser") ||
	    !strcasecmp(name, "max96724")) {
		*addr = DES_ADDR;
		return 0;
	}

	if (!strcasecmp(name, "ser") || !strcasecmp(name, "serializer") ||
	    !strcasecmp(name, "max9295")) {
		*addr = SER_BASE_ADDR + ctx->link + 1;
		return 0;
	}

	if (!strcasecmp(name, "isp") || !strcasecmp(name, "gw5200")) {
		*addr = ISP_ADDR;
		return 0;
	}

	if (!strcasecmp(name, "eeprom")) {
		*addr = EEPROM_ADDR;
		return 0;
	}

	if (!strcasecmp(name, "sensor") || !strcasecmp(name, "cam") ||
	    !strcasecmp(name, "imx390")) {
		*addr = SENSOR_ADDR;
		return 0;
	}

	if (!parse_u32(name, 0x7f, &val)) {
		*addr = val;
		return 0;
	}

	fprintf(stderr, "unknown target '%s' (use des, ser, sensor or 0xNN)\n",
		name);
	return -1;
}

static int cmd_read(struct gmsl_ctx *ctx, int argc, char **argv)
{
	unsigned int reg, count = 1, max_reg, i;
	int addr, ret;
	uint8_t val;

	if (argc < 2) {
		fprintf(stderr, "usage: read <target> <reg> [count]\n");
		return 1;
	}

	max_reg = ctx->reg_bytes == 2 ? MAX_REG_16 : MAX_REG_8;
	if (parse_u32(argv[1], max_reg, &reg))
		die("invalid register '%s'", argv[1]);
	if (argc >= 3 && parse_u32(argv[2], MAX_BYTES, &count))
		die("invalid count '%s'", argv[2]);
	if (!count || reg + count - 1 > max_reg)
		die("register range out of bounds");

	if (resolve_target(ctx, argv[0], &addr))
		return 1;

	for (i = 0; i < count; i++) {
		ret = i2c_read_byte(ctx, addr, reg + i, &val);
		if (ret) {
			fprintf(stderr, "%s@0x%02x ", argv[0], addr);
			print_reg(stderr, ctx, reg + i);
			fprintf(stderr, ": %s\n", strerror(-ret));
			return 1;
		}
		printf("%s@0x%02x ", argv[0], addr);
		print_reg(stdout, ctx, reg + i);
		printf(" = 0x%02x\n", val);
	}

	return 0;
}

static int cmd_write(struct gmsl_ctx *ctx, int argc, char **argv)
{
	unsigned int reg, val, max_reg;
	int addr, ret, i;
	uint8_t *vals;
	int nvals;

	if (argc < 3) {
		fprintf(stderr, "usage: write <target> <reg> <val> [val...]\n");
		return 1;
	}

	max_reg = ctx->reg_bytes == 2 ? MAX_REG_16 : MAX_REG_8;
	if (parse_u32(argv[1], max_reg, &reg))
		die("invalid register '%s'", argv[1]);

	nvals = argc - 2;
	if (!nvals || nvals > MAX_BYTES || reg + nvals - 1 > max_reg)
		die("invalid value range");

	if (resolve_target(ctx, argv[0], &addr))
		return 1;

	vals = calloc(nvals, sizeof(*vals));
	if (!vals)
		die("out of memory");
	for (i = 0; i < nvals; i++) {
		if (parse_u32(argv[i + 2], 0xff, &val))
			die("invalid value '%s'", argv[i + 2]);
		vals[i] = val;
	}

	for (i = 0; i < nvals; i++) {
		ret = i2c_write_byte(ctx, addr, reg + i, vals[i]);
		if (ret) {
			fprintf(stderr, "%s@0x%02x ", argv[0], addr);
			print_reg(stderr, ctx, reg + i);
			fprintf(stderr, ": %s\n", strerror(-ret));
			free(vals);
			return 1;
		}
		printf("%s@0x%02x ", argv[0], addr);
		print_reg(stdout, ctx, reg + i);
		printf(" <= 0x%02x\n", vals[i]);
	}

	free(vals);
	return 0;
}

static int cmd_dump(struct gmsl_ctx *ctx, int argc, char **argv)
{
	unsigned int start, end, reg, max_reg;
	int addr, ret, col, errors = 0;
	uint8_t val;

	if (argc < 2) {
		fprintf(stderr, "usage: dump <target> <start> [end]\n");
		return 1;
	}

	max_reg = ctx->reg_bytes == 2 ? MAX_REG_16 : MAX_REG_8;
	if (parse_u32(argv[1], max_reg, &start))
		die("invalid start register '%s'", argv[1]);
	end = start + 15;
	if (argc >= 3 && parse_u32(argv[2], max_reg, &end))
		die("invalid end register '%s'", argv[2]);
	if (end < start)
		die("end register is below start register");

	if (resolve_target(ctx, argv[0], &addr))
		return 1;

	for (reg = start; reg <= end; ) {
		print_reg(stdout, ctx, reg);
		printf(":");
		for (col = 0; col < 16 && reg <= end; col++, reg++) {
			ret = i2c_read_byte(ctx, addr, reg, &val);
			if (ret) {
				printf(" --");
				errors++;
				if (reg == start) {
					printf("\n%s@0x%02x: %s\n", argv[0], addr,
					       strerror(-ret));
					return 1;
				}
				continue;
			}
			printf(" %02x", val);
		}
		printf("\n");
	}

	if (errors) {
		fprintf(stderr, "%d register(s) failed to read\n", errors);
		return 1;
	}

	return 0;
}

static int cmd_links(struct gmsl_ctx *ctx)
{
	static const struct {
		const char *name;
		unsigned int reg;
	} links[] = {
		{ "A", 0x001a },
		{ "B", 0x000a },
		{ "C", 0x000b },
		{ "D", 0x000c },
	};
	unsigned int i;
	uint8_t val;
	int ret;

	for (i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
		ret = i2c_read_byte(ctx, DES_ADDR, links[i].reg, &val);
		if (ret) {
			fprintf(stderr, "link %s: %s\n", links[i].name,
				strerror(-ret));
			return 1;
		}
		printf("link %s: %s\n", links[i].name,
		       val & 0x08 ? "locked" : "unlocked");
	}

	ret = i2c_read_byte(ctx, DES_ADDR, 0x0006, &val);
	if (ret) {
		fprintf(stderr, "link mask: %s\n", strerror(-ret));
		return 1;
	}
	printf("link mask 0x0006 = 0x%02x\n", val);

	return 0;
}

static int cmd_scan(struct gmsl_ctx *ctx, int argc, char **argv)
{
	unsigned int start = 0x08, end = 0x77, addr;
	int saved_retries, ret;
	uint8_t val;

	if (argc >= 1 && parse_u32(argv[0], 0x7f, &start))
		die("invalid start address '%s'", argv[0]);
	if (argc >= 2 && parse_u32(argv[1], 0x7f, &end))
		die("invalid end address '%s'", argv[1]);
	if (end < start)
		die("end address is below start address");

	saved_retries = ctx->retries;
	ctx->retries = 0;
	printf("scan /dev/i2c-%d 0x%02x..0x%02x\n", ctx->bus, start, end);
	for (addr = start; addr <= end; addr++) {
		ret = i2c_read_byte(ctx, addr, 0x0000, &val);
		if (!ret)
			printf("  0x%02x: ACK (reg0=0x%02x)\n", addr, val);
	}
	ctx->retries = saved_retries;

	return 0;
}

static int cmd_detect(struct gmsl_ctx *ctx)
{
	uint8_t val;
	int addr, ret, found_ser = 0, saved_retries;

	printf("bus: /dev/i2c-%d\n", ctx->bus);

	ret = i2c_read_byte(ctx, DES_ADDR, 0x0000, &val);
	if (ret) {
		fprintf(stderr, "deserializer @0x%02x: %s\n", DES_ADDR,
			strerror(-ret));
		return 1;
	}
	printf("deserializer @0x%02x: id=0x%02x\n", DES_ADDR, val);

	cmd_links(ctx);

	saved_retries = ctx->retries;
	ctx->retries = 0;
	printf("serializer:");
	for (addr = SER_BASE_ADDR; addr <= SER_BASE_ADDR + 4; addr++) {
		ret = i2c_read_byte(ctx, addr, 0x000d, &val);
		if (ret)
			continue;
		printf(" @0x%02x(id=0x%02x)", addr, val);
		found_ser++;
	}
	if (!found_ser)
		printf(" none");
	printf("\n");
	ctx->retries = saved_retries;

	ret = i2c_read_byte(ctx, ISP_ADDR, 0x0000, &val);
	if (ret)
		printf("isp @0x%02x: no response (%s)\n", ISP_ADDR,
		       strerror(-ret));
	else
		printf("isp @0x%02x: reg0=0x%02x (I2C ACK, GW5200)\n",
		       ISP_ADDR, val);

	ret = i2c_read_byte(ctx, EEPROM_ADDR, 0x0000, &val);
	if (ret)
		printf("eeprom @0x%02x: no response (%s)\n", EEPROM_ADDR,
		       strerror(-ret));
	else
		printf("eeprom @0x%02x: reg0=0x%02x (I2C ACK)\n",
		       EEPROM_ADDR, val);

	ret = i2c_read_byte(ctx, SENSOR_ADDR, 0x0000, &val);
	if (ret) {
		printf("sensor @0x%02x: no response\n", SENSOR_ADDR);
		printf("  IMX390 is behind the GW5200 ISP on SG2-IMX390C; use 'isp' for camera control\n");
	} else {
		printf("sensor @0x%02x: reg0=0x%02x (I2C ACK)\n",
		       SENSOR_ADDR, val);
	}

	return 0;
}

static void usage(FILE *f)
{
	fprintf(f,
		"Usage: gmsl-reg [options] [command [args...]]\n"
		"\n"
		"Targets:\n"
		"  des                   MAX96724 deserializer @0x27\n"
		"  ser                   serializer @0x41+link (after driver probe)\n"
		"  isp                   GW5200 ISP @0x6d\n"
		"  eeprom                module EEPROM @0x51\n"
		"  sensor                IMX390 @0x21 (behind ISP on SG2-IMX390C)\n"
		"  0xNN                  raw 7-bit I2C address\n"
		"\n"
		"Commands:\n"
		"  detect                probe deserializer, serializer, ISP and sensor\n"
		"  scan [start] [end]    probe I2C addresses (default 0x08..0x77)\n"
		"  links                 show GMSL link lock state\n"
		"  read <target> <reg> [count]\n"
		"  write <target> <reg> <val> [val...]\n"
		"  dump <target> <start> [end]\n"
		"  help\n"
		"\n"
		"Options:\n"
		"  -b, --bus N           I2C bus (autodetect from the driver)\n"
		"  -l, --link N          GMSL link 0..3 for the ser alias (default 0)\n"
		"  -g, --reg-bits N      8 or 16-bit register addresses (default 16)\n"
		"  -t, --retries N       retry count (default 2)\n"
		"  -i, --interactive     interactive shell\n"
		"  -h, --help\n"
		"\n"
		"Examples:\n"
		"  gmsl-reg detect\n"
		"  gmsl-reg scan\n"
		"  gmsl-reg read des 0x0000\n"
		"  gmsl-reg read ser 0x000d\n"
		"  gmsl-reg read isp 0x0000 4\n"
		"  gmsl-reg dump des 0x0000 0x003f\n"
		"  gmsl-reg -i\n");
}

static int do_command(struct gmsl_ctx *ctx, int argc, char **argv)
{
	if (!argc)
		return 0;

	if (!strcmp(argv[0], "r") || !strcmp(argv[0], "read"))
		return cmd_read(ctx, argc - 1, argv + 1);
	if (!strcmp(argv[0], "w") || !strcmp(argv[0], "write"))
		return cmd_write(ctx, argc - 1, argv + 1);
	if (!strcmp(argv[0], "d") || !strcmp(argv[0], "dump"))
		return cmd_dump(ctx, argc - 1, argv + 1);
	if (!strcmp(argv[0], "detect"))
		return cmd_detect(ctx);
	if (!strcmp(argv[0], "scan"))
		return cmd_scan(ctx, argc - 1, argv + 1);
	if (!strcmp(argv[0], "links"))
		return cmd_links(ctx);
	if (!strcmp(argv[0], "help")) {
		usage(stdout);
		return 0;
	}

	fprintf(stderr, "unknown command '%s'\n", argv[0]);
	return 1;
}

static int shell(struct gmsl_ctx *ctx)
{
	char *line = NULL, *save, *tok, *argv[32];
	size_t cap = 0;
	int argc;

	printf("gmsl-reg: /dev/i2c-%d, link %c\n", ctx->bus, 'A' + ctx->link);
	printf("type 'help' for commands, 'quit' to exit\n");

	for (;;) {
		printf("gmsl> ");
		fflush(stdout);
		if (getline(&line, &cap, stdin) < 0)
			break;

		argc = 0;
		tok = strtok_r(line, " \t\r\n", &save);
		while (tok && argc < (int)(sizeof(argv) / sizeof(argv[0]))) {
			argv[argc++] = tok;
			tok = strtok_r(NULL, " \t\r\n", &save);
		}
		if (!argc)
			continue;
		if (!strcasecmp(argv[0], "quit") ||
		    !strcasecmp(argv[0], "exit"))
			break;

		do_command(ctx, argc, argv);
	}

	free(line);
	return 0;
}

int main(int argc, char **argv)
{
	struct gmsl_ctx ctx = {
		.fd = -1,
		.bus = -1,
		.link = 0,
		.reg_bytes = 2,
		.retries = 2,
	};
	int interactive = 0, opt, ret;
	unsigned int val;

	static const struct option long_opts[] = {
		{ "bus", required_argument, 0, 'b' },
		{ "link", required_argument, 0, 'l' },
		{ "reg-bits", required_argument, 0, 'g' },
		{ "retries", required_argument, 0, 't' },
		{ "interactive", no_argument, 0, 'i' },
		{ "help", no_argument, 0, 'h' },
		{ },
	};

	while ((opt = getopt_long(argc, argv, "b:l:g:t:ih", long_opts,
				  NULL)) != -1) {
		switch (opt) {
		case 'b':
			if (parse_u32(optarg, 999, &val))
				die("invalid bus '%s'", optarg);
			ctx.bus = val;
			break;
		case 'l':
			if (parse_u32(optarg, 3, &val))
				die("invalid link '%s'", optarg);
			ctx.link = val;
			break;
		case 'g':
			if (parse_u32(optarg, 16, &val) || (val != 8 && val != 16))
				die("reg-bits must be 8 or 16");
			ctx.reg_bytes = val / 8;
			break;
		case 't':
			if (parse_u32(optarg, 10, &val))
				die("invalid retry count '%s'", optarg);
			ctx.retries = val;
			break;
		case 'i':
			interactive = 1;
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 1;
		}
	}

	open_bus(&ctx);

	if (interactive || optind >= argc)
		ret = shell(&ctx);
	else
		ret = do_command(&ctx, argc - optind, argv + optind);

	close(ctx.fd);
	return ret ? 1 : 0;
}
