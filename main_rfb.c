
#include <sys/socket.h>
#include <sys/epoll.h>
#include <signal.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static int usage_main_rfb()
{
	printf("main_rfb <host> <port> \n");
	return -1;
}

int rfb_read_socket(int fd, uint8_t* data, uint32_t n)
{
	fd_set fds;
	struct timeval timeout;
	int retry = 3;
	uint32_t cnt = n;
	timeout.tv_sec=1;
	timeout.tv_usec=0;
	while(cnt > 0) {
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		select(fd+1, &fds, NULL,NULL, &timeout);
		if(FD_ISSET(fd, &fds)) {
			int rc = read(fd, data, cnt);
			if(rc < 0) {
				if(retry <= 0) {
					printf("errorno %d, %s\n", errno, strerror(errno));
					return -1;
				} else {
					printf("retry %d, %d, %d\n", retry, rc, n);
					retry--;
					continue;
				}
			}
			if(rc != n) {
				printf("read error rc:%d, %d != cnt:%d\n", rc, n, cnt);
			}
			data += rc;
			cnt-=rc;
			if(cnt != 0)
				printf("continue %d\n:", cnt);
		}
	}
	return n;
}

int rfb_write_socket(int fd, uint8_t* data, int n)
{
	int retry = 3;
	int cnt = n;
	int i = 0;
	while(cnt > 0) {
		int wc = write(fd, data + i, (cnt - i));
		if(wc < 0) {
			printf("errorno %d, %s\n", errno, strerror(errno));
			if(retry <= 0)
				return -1;
			else {
				printf("retry %d, %d, %d\n", retry, wc, n);
				retry--;
				continue;
			}
		}
		i += wc;
		if(wc == 0)
			break;
	}
	return i;
}

/*
 * RFB : remote framebuffer
 */
int rfb_create_socket(uint8_t *hostname, int port)
{
	int sock = -1;
	int one = 1;
	struct sockaddr_in sa_in;

	if ( (sock = socket (AF_INET, SOCK_STREAM, 0)) <0) {
		printf("socket created error\n");
	}

	sa_in.sin_addr.s_addr = inet_addr(hostname);//IP address
	sa_in.sin_port = htons(port);
	sa_in.sin_family = AF_INET;

	if (connect(sock,(struct sockaddr*) &sa_in, sizeof(sa_in)) < 0) {
		printf("Connect error\n");
		close(sock);
		return -1;
	}
	if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof(one)) < 0) {
		printf("setting socket options failed\n");
		close(sock);
		return -1;
	}
	if (fcntl(sock, F_SETFL, O_NONBLOCK) < 0) {
		printf("setting socket to non-blocking failed\n.");
		return -1;
	}

	return sock;
}
struct rfb_protocol_version_t {
	uint8_t rfb[3];
	uint8_t resv1;
	uint8_t major[3];
	uint8_t resv2;
	uint8_t minor[3];
	uint8_t resv3;
} __attribute__ ((packed));

int handle_protocol_version(int socket_fd)
{
	int rc = 0;
	int wc = 0;
	struct rfb_protocol_version_t rfb_pv;
	uint8_t *ptr = (uint8_t*)&rfb_pv;
	rc = rfb_read_socket(socket_fd, ptr, sizeof(struct rfb_protocol_version_t));
	if(rc < 0) {
		printf("handshake failed - protocol version\n");
		return -1;
	}
	wc = rfb_write_socket(socket_fd, ptr, sizeof(struct rfb_protocol_version_t));
	printf("Protocol version [%d,%d]-%s\n", rc, wc, ptr);
	return wc;

}

struct rfb_security_types_t {
	uint8_t count;
} __attribute__ ((packed));

enum {
	OK = 0,
	FAILED,
	FAILED_ATTEMP,
};

int handle_security(int socket_fd)
{
	int i = 0;
	int rc = 0;
	int wc = 0;
	struct rfb_security_types_t rfb_st;
	uint8_t *ptr = (uint8_t*)&rfb_st;
	uint32_t security_result = 1;
	uint8_t security = 1;
	rc = rfb_read_socket(socket_fd, ptr, sizeof(struct rfb_security_types_t));
	if(rc < 0) {
		printf("handle security error\n");
		return -1;
	}
	printf("security %d, %d, %x\n", rc, wc, rfb_st.count);

	for(i=0;i<rfb_st.count;i++){
		rfb_read_socket(socket_fd, &security, sizeof(security));
		printf("%d\n", security);
	}

	security = 1;
	wc = rfb_write_socket(socket_fd, &security, sizeof(security));

	printf("write %d\n", wc);

	rc = rfb_read_socket(socket_fd, (uint8_t*)&security_result, sizeof(security_result));

	switch(security_result){
	case OK:
		printf("security result OK\n");
		return 0;
	break;
	default:
		printf("security result failed %x\n", security_result);
	break;
	}

	return -1;
}

struct client_init_t {
	uint8_t shared_flag;
} __attribute__ ((packed));

struct pixel_format_t {
	uint8_t bit_per_pixel;
	uint8_t depth;
	uint8_t big_endian;
	uint8_t true_color;
	uint16_t red_max;
	uint16_t green_max;
	uint16_t blue_max;
	uint8_t red_shift;
	uint8_t green_shift;
	uint8_t blue_shift;
	uint8_t padding[3];
} __attribute__ ((packed));

#define NAME_LEN	30
struct server_init_t {
	uint16_t fb_width;
	uint16_t fb_height;
	struct pixel_format_t fb_pixel_format;
	uint32_t name_len;
	uint8_t name[NAME_LEN];
} __attribute__ ((packed));

int rfb_init_msg(int socket_fd, struct server_init_t *si)
{
	int rc = 0;
	int wc = 0;
	uint8_t *p_name = NULL;
	struct client_init_t client_init;
	struct server_init_t server_init;
	client_init.shared_flag = 0;
	wc = rfb_write_socket(socket_fd, (uint8_t*)&client_init, sizeof(client_init));
	printf("ci:%d-%d\n", rc, client_init.shared_flag);
	rc = rfb_read_socket(socket_fd, (uint8_t*)&server_init, (sizeof(server_init) - NAME_LEN));

	memset(si, 0, sizeof(server_init));
	si->fb_width = ntohs(server_init.fb_width);
	si->fb_height = ntohs(server_init.fb_height);
	si->fb_pixel_format.bit_per_pixel = server_init.fb_pixel_format.bit_per_pixel;
	si->fb_pixel_format.depth = server_init.fb_pixel_format.depth;
	si->fb_pixel_format.big_endian = server_init.fb_pixel_format.big_endian;
	si->fb_pixel_format.true_color = server_init.fb_pixel_format.true_color;
	si->fb_pixel_format.red_max = ntohs(server_init.fb_pixel_format.red_max);
	si->fb_pixel_format.green_max = ntohs(server_init.fb_pixel_format.green_max);
	si->fb_pixel_format.blue_max = ntohs(server_init.fb_pixel_format.blue_max);
	si->fb_pixel_format.red_shift = server_init.fb_pixel_format.red_shift;
	si->fb_pixel_format.green_shift = server_init.fb_pixel_format.green_shift;
	si->fb_pixel_format.blue_shift = server_init.fb_pixel_format.blue_shift;
	si->name_len = ntohl(server_init.name_len);
	p_name = malloc(si->name_len);
	memset(p_name, 0, si->name_len);
	rc = rfb_read_socket(socket_fd, p_name, si->name_len);
	if(NAME_LEN > si->name_len){
		memcpy(si->name, p_name, si->name_len);
	} else {
		memcpy(si->name, p_name, NAME_LEN - 1);
	}
	free(p_name);
	p_name = NULL;

	printf("si:(%d x %d), [%d, %d, %d, %x, %d, %d, %d, %d, %d, %d], %d, %s\n", si->fb_width, si->fb_height,
			si->fb_pixel_format.bit_per_pixel,
			si->fb_pixel_format.depth,
			si->fb_pixel_format.big_endian,
			si->fb_pixel_format.true_color,
			si->fb_pixel_format.red_max,
			si->fb_pixel_format.green_max,
			si->fb_pixel_format.blue_max,
			si->fb_pixel_format.red_shift,
			si->fb_pixel_format.green_shift,
			si->fb_pixel_format.blue_shift,
			si->name_len, si->name);

	return 0;
}

#define MSG_FRAMEBUFFER_UPDATE_REQUEST	3
#define MSG_FRAMEBUFFER_UPDATE		0
#define MSG_RING_BELL			2
#define MSG_SET_COLOR_MAP_ENTRY		1

struct framebuffer_update_request_t {
	uint8_t msg_type;
	uint8_t incremental;
	uint16_t x_pos;
	uint16_t y_pos;
	uint16_t width;
	uint16_t height;
} __attribute__ ((packed));

int rfb_request_entire_content(int socket_fd, struct server_init_t *si)
{
	int wc = 0;
	struct framebuffer_update_request_t fb_up_req;
	fb_up_req.msg_type = MSG_FRAMEBUFFER_UPDATE_REQUEST;
	fb_up_req.incremental = 0;
	fb_up_req.x_pos = 0;
	fb_up_req.y_pos = 0;
	fb_up_req.width = htons(si->fb_width);
	fb_up_req.height = htons(si->fb_height);
	wc = rfb_write_socket(socket_fd, (uint8_t*)&fb_up_req, sizeof(struct framebuffer_update_request_t));
	return wc;
}

int rfb_request_changed_content(int socket_fd, struct server_init_t *si)
{
	int wc = 0;
	struct framebuffer_update_request_t fb_up_req;
	fb_up_req.msg_type = MSG_FRAMEBUFFER_UPDATE_REQUEST;
	fb_up_req.incremental = 1;
	fb_up_req.x_pos = 0;
	fb_up_req.y_pos = 0;
	fb_up_req.width = htons(si->fb_width);
	fb_up_req.height = htons(si->fb_height);
	wc = rfb_write_socket(socket_fd, (uint8_t*)&fb_up_req, sizeof(struct framebuffer_update_request_t));
	return wc;
}

int rfb_handshake(int socket_fd, struct server_init_t *si)
{
	int cnt = 0;
	if(handle_protocol_version(socket_fd) > 0) {
		if(handle_security(socket_fd) == 0) {
			rfb_init_msg(socket_fd, si);
			return 0;
		}

	}

	return -1;
}

struct framebuffer_update_header_t {
	uint8_t padding;
	uint16_t no_rectangle;
} __attribute__ ((packed));

struct framebuffer_update_t {
	uint16_t x_pos;
	uint16_t y_pos;
	uint16_t width;
	uint16_t height;
	uint32_t encoding_type;
} __attribute__ ((packed));

struct color_map_header_t {
	uint8_t padding;
	uint16_t first_color;
	uint16_t no_colors;
} __attribute__ ((packed));

struct color_map_t {
	uint16_t red;
	uint16_t green;
	uint16_t blue;
} __attribute__ ((packed));

#define ENCODING_RAW	0
int rfb_processor(int socket_fd, struct server_init_t *si)
{
	int i = 0;
	int rc = 0;
	uint8_t msg_type = 0;
	struct framebuffer_update_header_t fb_up_head;
	struct framebuffer_update_t _fb_up;
	struct framebuffer_update_t fb_up;

	struct color_map_header_t color_map_head;
	struct color_map_t _color_map;
	struct color_map_t color_map;

	uint16_t no_of = 0;
	uint8_t row_buf[4096*4] = {0};

	rc = rfb_read_socket(socket_fd, (uint8_t*)&msg_type, sizeof(msg_type));

	if(rc < 0) {
		printf("errorno %d, %s\n", errno, strerror(errno));
		return -1;
	}

	//printf("msg:%x\n", msg_type);
	switch(msg_type) {
	case MSG_FRAMEBUFFER_UPDATE:
		rc = rfb_read_socket(socket_fd, (uint8_t*)&fb_up_head, sizeof(struct framebuffer_update_header_t));
		no_of = ntohs(fb_up_head.no_rectangle);
		if(fb_up_head.padding != 0) {
			printf("padding Not 0\n");
			printf("fb up %x, no: %d\n", fb_up_head.padding, no_of);
			return -1;
		}
		for(i=0;i<no_of;i++) {
			rc = rfb_read_socket(socket_fd, (uint8_t*)&_fb_up, sizeof(struct framebuffer_update_t));
			fb_up.x_pos = ntohs(_fb_up.x_pos);
			fb_up.y_pos = ntohs(_fb_up.y_pos);
			fb_up.width = ntohs(_fb_up.width);
			fb_up.height = ntohs(_fb_up.height);
			fb_up.encoding_type = ntohl(_fb_up.encoding_type);
			//printf("%d/%d-[x:%d,y:%d], [w:%d,h:%d], enc:%x\n", i, no_of, fb_up.x_pos, fb_up.y_pos, fb_up.width, fb_up.height, fb_up.encoding_type);
			uint32_t p_rect_len = (fb_up.width) * (si->fb_pixel_format.bit_per_pixel/8);
			uint8_t *p_rect = &row_buf[0];
			//uint8_t out_name[50] = {0};
			//int fd = -1;
			switch(fb_up.encoding_type){
			case ENCODING_RAW:
				for(int pos=0;pos<fb_up.height;pos++){
					rc = rfb_read_socket(socket_fd, p_rect, p_rect_len);
					if(rc != p_rect_len) {
						printf("rc != p_rect_len\n");
					}
#if 0
				sprintf(out_name, "/tmp/test_%d_%d_%d_%d.raw", fb_up.x_pos, fb_up.y_pos, fb_up.width, fb_up.height);
				fd = open(out_name, O_RDWR|O_CREAT);
				write(fd, p_rect, p_rect_len);
				close(fd);
#endif
				}
			break;
			default:
				printf("encoding type %d\n", fb_up.encoding_type);
				return -1;
			break;
			}
		}
	break;
	case MSG_RING_BELL:
		printf("Ring Bell\n");
	break;
	case MSG_SET_COLOR_MAP_ENTRY:
		rc = rfb_read_socket(socket_fd, (uint8_t*)&color_map_head, sizeof(struct color_map_header_t));
		no_of = ntohs(color_map_head.no_colors);
		printf("Color map %d, %d, %d\n", color_map_head.padding, ntohs(color_map_head.first_color), no_of);
		for(i=0;i<no_of;i++) {
			rc = rfb_read_socket(socket_fd, (uint8_t*)&_color_map, sizeof(struct color_map_t));
			color_map.red = ntohs(_color_map.red);
			color_map.green = ntohs(_color_map.green);
			color_map.blue = ntohs(_color_map.blue);
			printf("[r:%x,g:%x,b:%x]\n", color_map.red, color_map.green, color_map.blue);
		}
	break;
	default:
		printf("msg %d\n", msg_type);
		return -1;
	break;
	}
	return 0;
}

int main(int argc, char** argv)
{
	int socket_fd = -1;
	struct server_init_t si;
	if(argc < 3) {
		return usage_main_rfb();
	}

	socket_fd = rfb_create_socket(argv[1], strtol(argv[2], NULL, 10));

	if(rfb_handshake(socket_fd, &si) < 0){
		printf("handleshake failed....\n");
		close(socket_fd);
		return -1;
	}
	if(rfb_request_entire_content(socket_fd, &si) < 0){
		printf("request content failed....\n");
		close(socket_fd);
		return -1;
	}
	do {
		if(rfb_processor(socket_fd, &si) < 0)
			break;
		if(rfb_request_changed_content(socket_fd, &si) < 0){
			printf("request content failed....\n");
			break;
		}
	} while(1);
	close(socket_fd);
	return 0;
}

