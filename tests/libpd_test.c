 /**
  * Copyright 2016 Comcast Cable Communications Management, LLC
  *
  * Licensed under the Apache License, Version 2.0 (the "License");
  * you may not use this file except in compliance with the License.
  * You may obtain a copy of the License at
  *
  *     http://www.apache.org/licenses/LICENSE-2.0
  *
  * Unless required by applicable law or agreed to in writing, software
  * distributed under the License is distributed on an "AS IS" BASIS,
  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  * See the License for the specific language governing permissions and
  * limitations under the License.
  *
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <CUnit/Basic.h>
#include <stdbool.h>

#include "../src/libparodus.h"
#include "../src/libparodus_private.h"
#include "../src/libparodus_time.h"
#include "../src/libparodus_queues.h"
#include <pthread.h>

#define MOCK_MSG_COUNT 10
#define MOCK_MSG_COUNT_STR "10"
#define NUM_KEEP_ALIVE_MSGS 5
#define TESTS_DIR_TAIL "/tests"
#define BUILD_DIR_TAIL "/build"
#define BUILD_TESTS_DIR_TAIL "/build/tests"
#define END_PIPE_MSG  "--END--\n"
#define SEND_EVENT_MSGS 1

//#define TCP_URL(ip) "tcp://" ip

#define TEST_RCV_URL "tcp://127.0.0.1:6006"
#define BAD_RCV_URL  "tcp://127.0.0.1:X006"
#define BAD_CLIENT_URL "tcp://127.0.0.1:X006"
#define GOOD_CLIENT_URL "tcp://127.0.0.1:6667"
#define GOOD_CLIENT_URL2 "tcp://127.0.0.1:6665"
//#define PARODUS_URL "ipc:///tmp/parodus_server.ipc"
//#define UNAVAIL_PARODUS_URL "tcp://10.172.47.109:6666"
#define TEST_SEND_URL  "tcp://127.0.0.1:6007"
#define BAD_SEND_URL   "tcp://127.0.0.1:x007"
#define BAD_PARODUS_URL "tcp://127.0.0.1:x007"
#define GOOD_PARODUS_URL "tcp://127.0.0.1:6666"
#define CONNECT_ON_EVERY_SEND_URL "test:tcp://127.0.0.1:6666"
//#define CLIENT_URL "ipc:///tmp/parodus_client.ipc"

static char current_dir_buf[256];

#define CURRENT_DIR_IS_BUILD	0
#define CURRENT_DIR_IS_TESTS	1
#define CURRENT_DIR_IS_BUILD_TESTS	2

static int current_dir_id = CURRENT_DIR_IS_BUILD;


#define RUN_TESTS_NAME(name) ( \
  (current_dir_id == CURRENT_DIR_IS_TESTS) ? "./" name : \
  (current_dir_id == CURRENT_DIR_IS_BUILD) ? "../tests/" name : \
  "../../tests/" name )   

#define BUILD_TESTS_NAME(name) ( \
  (current_dir_id == CURRENT_DIR_IS_TESTS) ? "../build/tests/" name : \
  (current_dir_id == CURRENT_DIR_IS_BUILD) ? "./tests/" name : \
  "./" name )


#define END_PIPE_NAME() RUN_TESTS_NAME("mock_parodus_end.txt")
#define MOCK_PARODUS_PATH() BUILD_TESTS_NAME("mock_parodus")
#define TEST_MSGS_PATH() RUN_TESTS_NAME("parodus_test_msgs.txt")

static int end_pipe_fd = -1;
static char *end_pipe_msg = END_PIPE_MSG;

static void initEndKeypressHandler();
static void *endKeypressHandlerTask();

static pthread_t endKeypressThreadId;
static pthread_t SecondReceiverThreadId;

static const char *service_name1 = "iot";
static const char *service_name2 = "config";

static bool using_mock = false;
static bool no_mock_send_only_test = false;
static bool do_send_blocking_test = false;
static bool do_send_disconnect_test = false;
static bool do_multiple_inst_test = false;
static bool do_multiple_rcv_test = false;
static bool connect_on_every_send = false;
static bool do_multiple_inits_test = false;
static bool switch_service_names = false;

static libpd_instance_t test_instance1;
static libpd_instance_t test_instance2;

// libparodus functions to be tested
extern void test_set_cfg (libpd_cfg_t *new_cfg);
extern int flush_wrp_queue (libpd_mq_t wrp_queue, uint32_t delay_ms, int *oserr);
extern int connect_receiver 
	(const char *rcv_url, int keepalive_timeout_secs, int *oserr);
extern int connect_sender (const char *send_url, int *oserr);
extern void shutdown_socket (int *sock);

extern bool is_auth_received (void);
extern int libparodus_receive__ (libpd_mq_t wrp_queue, 
	wrp_msg_t **msg, uint32_t ms, int *oserr);

// libparodus_log functions to be tested
extern int get_valid_file_num (const char *file_name, const char *date);
extern int get_last_file_num_in_dir (const char *date, const char *log_dir);

// test helper functions defined in libparodus
extern int test_create_wrp_queue 
	(libpd_mq_t *wrp_queue, const char *wrp_queue_name, int *oserr);
extern void test_close_wrp_queue (libpd_mq_t *wrp_queue);
extern int  test_close_receiver (libpd_instance_t instance, int *oserr);
extern void test_send_wrp_queue_ok (libpd_mq_t wrp_queue, int *oserr);
extern void test_get_counts (libpd_instance_t instance, 
	int *keep_alive_count, int *reconnect_count);


#if TEST_ENVIRONMENT==2
bool CheckLevel (int level)
{
	int err = 0;
	const char *level_name;
	char timestamp[TIMESTAMP_BUFLEN];

	err = make_current_timestamp (timestamp);
	if (err != 0)
		timestamp[0] = '\0';

  if ((level) == LEVEL_ERROR)
    level_name = "Error: ";
  else if ((level) == LEVEL_INFO)
    level_name = "Info: ";
  else
    level_name = "Debug: ";

	printf ("[%s] %s: ", timestamp, level_name);
	return true;
}

int Printf (const char *msg, ...)
{
	va_list arg_ptr; 
	va_start(arg_ptr, msg); 
	vprintf (msg, arg_ptr);
	va_end (arg_ptr);
	return 0;
}

#endif

int check_current_dir (void)
{
	int current_dir_len, tail_len;
	char *pos, *end_pos;
	char *current_dir = getcwd (current_dir_buf, 256);

	if (NULL == current_dir) {
		libpd_log_err (LEVEL_ERROR, errno, ("Unable to get current directory\n"));
		return -1;
	}

	printf ("Current dir in libpd test is %s\n", current_dir);

	current_dir_len = strlen (current_dir_buf);
	end_pos = current_dir + current_dir_len;

	tail_len = strlen (BUILD_TESTS_DIR_TAIL);
	pos = end_pos - tail_len;
	if (strcmp (pos, BUILD_TESTS_DIR_TAIL) == 0) {
		current_dir_id = CURRENT_DIR_IS_BUILD_TESTS;
		return 0;
	}

	tail_len = strlen (TESTS_DIR_TAIL);
	pos = end_pos - tail_len;
	if (strcmp (pos, TESTS_DIR_TAIL) == 0) {
		current_dir_id = CURRENT_DIR_IS_TESTS;
		return 0;
	}

	tail_len = strlen (BUILD_DIR_TAIL);
	pos = end_pos - tail_len;
	if (strcmp (pos, BUILD_DIR_TAIL) == 0) {
		current_dir_id = CURRENT_DIR_IS_BUILD;
		return 0;
	}

	libpd_log (LEVEL_ERROR, ("Not in parodus lib tests or build directory\n"));
	return -1;
}

static int create_end_pipe (void)
{
	const char *end_pipe_name = END_PIPE_NAME();
	int err = remove (end_pipe_name);
	if ((err != 0) && (errno != ENOENT)) {
		libpd_log_err (LEVEL_ERROR, errno, ("Error removing pipe %s\n", end_pipe_name));
		return -1;
	}
	libpd_log (LEVEL_INFO, ("LIBPD TEST: Removed pipe %s\n", end_pipe_name));
	err = mkfifo (end_pipe_name, 0666);
	if (err != 0) {
		libpd_log_err (LEVEL_ERROR, errno, ("Error creating pipe %s\n", end_pipe_name));
		return -1;
	}
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: Created fifo %s\n", end_pipe_name));
	return 0;
}

static int open_end_pipe (void)
{
	const char *end_pipe_name = END_PIPE_NAME();
	end_pipe_fd = open (end_pipe_name, O_WRONLY, 0222);
	if (end_pipe_fd == -1) {
		libpd_log_err (LEVEL_ERROR, errno, ("Error opening pipe %s\n", end_pipe_name));
		return -1;
	}
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: Opened fifo %s\n", end_pipe_name));
	return 0;
}

static int write_end_pipe (void)
{
#ifdef TEST_ENVIRONMENT
	const char *end_pipe_name = END_PIPE_NAME();
#endif
	int rtn, fd_flags;

	fd_flags = fcntl (end_pipe_fd, F_GETFL);
	if (fd_flags < 0) {
		libpd_log_err (LEVEL_ERROR, errno, ("Error retrieving pipe %s flags\n", end_pipe_name));
		return -1;
	}
	rtn = fcntl (end_pipe_fd, F_SETFL, fd_flags | O_NONBLOCK);
	if (rtn < 0) {
		libpd_log_err (LEVEL_ERROR, errno, ("Error setting pipe %s flags\n", end_pipe_name));
		return -1;
	}
	rtn = write (end_pipe_fd, end_pipe_msg, strlen (end_pipe_msg));
	if (rtn <= 0) {
		libpd_log_err (LEVEL_ERROR, errno, ("Error writing pipe %s\n", end_pipe_name));
		return -1;
	}
	return 0;
}

void show_src_dest_payload (char *src, char *dest, void *payload, size_t payload_size)
{
	size_t i;
	char *payload_str = (char *) payload;
	printf (" SOURCE:  %s\n", src);
	printf (" DEST  :  %s\n", dest);
	printf (" PAYLOAD: ");
	for (i=0; i<payload_size; i++)
		putchar (payload_str[i]);
	putchar ('\n');
}

void show_wrp_req_msg (struct wrp_req_msg *msg)
{
	show_src_dest_payload (msg->source, msg->dest, msg->payload, msg->payload_size);
}

void show_wrp_event_msg (struct wrp_event_msg *msg)
{
	show_src_dest_payload (msg->source, msg->dest, msg->payload, msg->payload_size);
}

void show_wrp_msg (wrp_msg_t *wrp_msg, int rcvr)
{
#ifndef TEST_ENVIRONMENT
	(void) (rcvr);
#endif
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: Rcvr %d Received WRP Msg type %d\n", 
		rcvr, wrp_msg->msg_type));
	if (wrp_msg->msg_type == WRP_MSG_TYPE__REQ) {
		show_wrp_req_msg (&wrp_msg->u.req);
		return;
	}
	if (wrp_msg->msg_type == WRP_MSG_TYPE__EVENT) {
		show_wrp_event_msg (&wrp_msg->u.event);
		return;
	}
	return;
}

int send_reply (libpd_instance_t instance, wrp_msg_t *wrp_msg)
{
	size_t i;
	size_t payload_size = wrp_msg->u.req.payload_size;
	char *payload = (char *) wrp_msg->u.req.payload;
	char *temp;
	// swap source and dest
	temp = wrp_msg->u.req.source;
	wrp_msg->u.req.source = wrp_msg->u.req.dest;
	wrp_msg->u.req.dest = temp;
	// Alter the payload
	for (i=0; i<payload_size; i++)
		payload[i] = tolower (payload[i]);
	return libparodus_send (instance, wrp_msg);
}

char *new_str (const char *str)
{
	char *buf = malloc (strlen(str) + 1);
	if (NULL == buf)
		return NULL;
	strcpy (buf, str);
	return buf;
}

void insert_number_into_buf (char *buf, unsigned num)
{
	char *pos = strrchr (buf, '#');
	if (NULL == pos)
		return;
	while (true) {
		*pos = (num%10) + '0';
		num /= 10;
		if (pos <= buf)
			break;
		pos--;
		if (*pos != '#')
			break;
	}
}

int send_event_msg (const char *src, const char *dest, 
	const char *payload, unsigned event_num, unsigned every)
{
	int rtn1 = 0;
	int rtn2 = 0;
	char *payload_buf;
	wrp_msg_t *new_msg;

#ifndef SEND_EVENT_MSGS
	return 0;
#endif
	new_msg = malloc (sizeof (wrp_msg_t));
	if (NULL == new_msg)
		return -1;
	if ((every == 0) || (every == 1) || ((event_num % every) == 0)) {
		libpd_log (LEVEL_INFO, ("Making event msg\n"));
	}
	memset ((void*) new_msg, 0, sizeof(wrp_msg_t));
	new_msg->msg_type = WRP_MSG_TYPE__EVENT;
	new_msg->u.event.source = new_str (src);
	new_msg->u.event.dest = new_str (dest);
	payload_buf = new_str (payload);
	insert_number_into_buf (payload_buf, event_num);
	new_msg->u.event.payload = (void*) payload_buf;
	new_msg->u.event.payload_size = strlen (payload) + 1;
	if ((every == 0) || (every == 1) || ((event_num % every) == 0)) {
		libpd_log (LEVEL_INFO, ("Sending event msg %u\n", event_num));
	}
	rtn1 = libparodus_send (test_instance1, new_msg);
	if (every == 1)
		rtn2 = libparodus_send (test_instance2, new_msg);
	//printf ("Freeing event msg\n");
	wrp_free_struct (new_msg);
	//printf ("Freed event msg\n");
	if (rtn1 != 0)
		return rtn1;
	return rtn2;
}

int send_event_msgs (unsigned *msg_num, unsigned *event_num, int count,
	bool both)
{
	int i;
	int send_flag = 0;
	unsigned msg_num_mod;

#ifndef SEND_EVENT_MSGS
	return 0;
#endif
	if (NULL != msg_num) {
		(*msg_num)++;
		msg_num_mod = (*msg_num) % 3;
		if (msg_num_mod != 0)
			return 0;
	}
	if (both)
		send_flag = 1;
	for (i=0; i<count; i++) {
		(*event_num)++;
		if (send_event_msg ("---LIBPARODUS---", "---ParodusService---",
			"---EventMessagePayload####", *event_num, send_flag) != 0)
			return -1;
	}
	return 0;
}

void test_send_blocking (void)
{
	unsigned event_num = 0;
	unsigned suspend_time = 15;
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: Begin Send Blocking Test\n"));
	int rtn =	send_event_msg ("---LIBPARODUS---", "---ParodusService---",
			"---SuspendReceive ####", suspend_time, 0);
	CU_ASSERT (rtn == 0);
	if (rtn != 0)
		return;
	while (true) {
		event_num++;
		rtn =	send_event_msg ("---LIBPARODUS---", "---ParodusService---",
			"---SendBlockTest ####", event_num, 100);
		if (rtn != 0)
			break;
	}
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: End Send Blocking Test\n"));
	sleep (5);
}

void test_send_disconnect (void)
{
	unsigned event_num = 0;
	unsigned disconnect_time = 15;
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: Begin Send Disconnect Test\n"));
	int rtn =	send_event_msg ("---LIBPARODUS---", "---ParodusService---",
			"---DisconnectReceive ####", disconnect_time, 0);
	CU_ASSERT (rtn == 0);
	if (rtn != 0)
		return;
	while (true) {
		event_num++;
		rtn =	send_event_msg ("---LIBPARODUS---", "---ParodusService---",
			"---SendBlockTest ####", event_num, 100);
		if (rtn != 0)
			break;
	}
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: End Send Disconnect Test\n"));
	sleep (5);
}

int start_mock_parodus ()
{
	int pid;
	const char *mock_parodus_path = MOCK_PARODUS_PATH();
	const char *test_msgs_path = TEST_MSGS_PATH();

	pid = fork ();
	if (pid == -1) {
		libpd_log_err (LEVEL_ERROR, errno, ("Fork failed\n"));
		CU_ASSERT_FATAL (-1 != pid);
	}
	if (pid == 0) {
		// child
		int err = execlp (mock_parodus_path, mock_parodus_path, 
			"-f", test_msgs_path, "-d", "3",
#ifdef MOCK_MSG_COUNT
			"-c", MOCK_MSG_COUNT_STR,
#endif
			(char*)NULL);
		if (err != 0) {
			libpd_log_err (LEVEL_ERROR, errno, ("Failed execlp of mock_parodus\n"));
		}
		libpd_log (LEVEL_ERROR, ("Child finished\n"));
	}
	return pid;	
}

void test_time (void)
{
	int rtn;
	char timestamp[20];
	struct timespec ts1;
	struct timespec ts2;
	bool ts2_greater;

	rtn = make_current_timestamp (timestamp);
	if (rtn == 0)
		printf ("LIBPD_TEST: Current time is %s\n", timestamp);
	else
		printf ("LIBPD_TEST: make timestamp error %d\n", rtn);
	CU_ASSERT (rtn == 0);
	rtn = get_expire_time (500, &ts1);
	CU_ASSERT (rtn == 0);
	rtn = get_expire_time (500, &ts2);
	CU_ASSERT (rtn == 0);
	printf ("ts1: (%u, %u), ts2: (%u, %u)\n", 
		(unsigned)ts1.tv_sec, (unsigned)ts1.tv_nsec,
		(unsigned)ts2.tv_sec, (unsigned)ts2.tv_nsec);
	if (ts2.tv_sec != ts1.tv_sec)
		ts2_greater = (bool) (ts2.tv_sec > ts1.tv_sec);
	else
		ts2_greater = (bool) (ts2.tv_nsec >= ts1.tv_nsec);
	CU_ASSERT (ts2_greater);
	rtn = get_expire_time (5000, &ts1);
	CU_ASSERT (rtn == 0);
	rtn = get_expire_time (500, &ts2);
	CU_ASSERT (rtn == 0);
	if (ts2.tv_sec != ts1.tv_sec)
		ts2_greater = (bool) (ts2.tv_sec > ts1.tv_sec);
	else
		ts2_greater = (bool) (ts2.tv_nsec >= ts1.tv_nsec);
	CU_ASSERT (!ts2_greater);
}

void test_queue_send_msg (libpd_mq_t q, unsigned timeout_ms, int n)
{
	void *msg;
	int msgsize, exterr;
	char msgbuf[100];

	sprintf (msgbuf, "Test Message # %d\n", n);
	msgsize = strlen(msgbuf) + 1;
	msg = malloc (msgsize);
	CU_ASSERT_FATAL (msg != NULL);
	strncpy ((char*)msg, msgbuf, msgsize);
	CU_ASSERT (libpd_qsend (q, msg, timeout_ms, &exterr) == 0);
}

int get_msg_num (const char *msg)
{
	int num = -1;
	bool found_pound = false;
	int i;
	char c;

	for (i=0; (c=msg[i]) != 0; i++)
	{
		if (!found_pound) {
			if (c == '#')
				found_pound = true;
			continue;
		}
		if ((c>='0') && (c<='9')) {
			if (num == -1)
				num = c - '0';
			else
				num = 10*num + (c - '0');
		}
	}
	return num;
}

void test_queue_rcv_msg (libpd_mq_t q, unsigned timeout_ms, int n)
{
	int err, msg_num, exterr;
	void *msg;
	err = libpd_qreceive (q, &msg, timeout_ms, &exterr);
	CU_ASSERT (err == 0);
	if (err != 0)
		return;
	fputs ((char*) msg, stdout);
	if (n < 0) {
		free (msg);
		return;
	}
	msg_num = get_msg_num ((char*)msg);
	free (msg);
	CU_ASSERT (msg_num >= 0);
	if (msg_num < 0)
		return;
	CU_ASSERT (msg_num == n);
}

static int flush_queue_count = 0;

void qfree (void * msg)
{
	flush_queue_count++;
	free (msg);
}

typedef struct {
	libpd_mq_t queue;
	unsigned initial_wait_ms;
	unsigned num_msgs;
	unsigned send_interval_ms;
} test_queue_info_t;

static void *test_queue_sender_thread (void *arg)
{
	test_queue_info_t *qinfo = (test_queue_info_t *) arg;
	unsigned i;

	libpd_log (LEVEL_INFO, ("LIBPD_TEST: started test_queue_sender_thread\n"));
	if (qinfo->initial_wait_ms != 0)
		delay_ms (qinfo->initial_wait_ms);

	for (i=0; i<qinfo->num_msgs; i++)
	{
		test_queue_send_msg (qinfo->queue, qinfo->send_interval_ms, i);
	}
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: ended test_queue_sender_thread\n"));
	return NULL;
}

void test_queues (void)
{
	test_queue_info_t qinfo;
	int i, rtn, exterr;
	void *msg;
	pthread_t sender_test_tid;

	qinfo.initial_wait_ms = 2000;
	qinfo.num_msgs = 5;
	qinfo.send_interval_ms = 500;

	CU_ASSERT (libpd_qcreate (&qinfo.queue, "//TEST_QUEUE", 5, &exterr) == 0);
	for (i=0; i< 5; i++)
		test_queue_send_msg (qinfo.queue, qinfo.send_interval_ms, i);
	CU_ASSERT (libpd_qsend (qinfo.queue, "extra message", 
		qinfo.send_interval_ms, &exterr) == 1); // timed out
	for (i=0; i< 5; i++)
		test_queue_rcv_msg (qinfo.queue, qinfo.send_interval_ms, i);
	CU_ASSERT (libpd_qreceive (qinfo.queue, &msg, 
		qinfo.send_interval_ms, &exterr) == 1); // timed out
	for (i=0; i< 5; i++)
	{
		test_queue_send_msg (qinfo.queue, qinfo.send_interval_ms, i);
		test_queue_rcv_msg (qinfo.queue, qinfo.send_interval_ms, i);
	}
	flush_queue_count = 0;
	CU_ASSERT (libpd_qdestroy (&qinfo.queue, &qfree) == 0);
	CU_ASSERT (flush_queue_count == 0);

	CU_ASSERT (libpd_qcreate (&qinfo.queue, "//TEST_QUEUE", 5, &exterr) == 0);
	for (i=0; i< 5; i++)
		test_queue_send_msg (qinfo.queue, qinfo.send_interval_ms, i);
	flush_queue_count = 0;
	CU_ASSERT (libpd_qdestroy (&qinfo.queue, &qfree) == 0);
	CU_ASSERT (flush_queue_count == 5);

	CU_ASSERT (libpd_qcreate (&qinfo.queue, "//TEST_QUEUE", 
		qinfo.num_msgs, &exterr) == 0);
	rtn = pthread_create 
		(&sender_test_tid, NULL, test_queue_sender_thread, (void*) &qinfo);
	CU_ASSERT (rtn == 0);
	if (rtn == 0) {
		for (i=0; i< (int)qinfo.num_msgs; i++)
			test_queue_rcv_msg (qinfo.queue, 4000, -1);
		pthread_join (sender_test_tid, NULL);
	}
	flush_queue_count = 0;
	CU_ASSERT (libpd_qdestroy (&qinfo.queue, &qfree) == 0);
	CU_ASSERT (flush_queue_count == 0);

	CU_ASSERT (libpd_qcreate (&qinfo.queue, "//TEST_QUEUE", 
		qinfo.num_msgs, &exterr) == 0);
	qinfo.initial_wait_ms = 0;
	qinfo.num_msgs += 5;
	qinfo.send_interval_ms = 5000;
	rtn = pthread_create 
		(&sender_test_tid, NULL, test_queue_sender_thread, (void*) &qinfo);
	CU_ASSERT (rtn == 0);
	if (rtn == 0) {
		delay_ms (2000);
		for (i=0; i< (int)qinfo.num_msgs; i++) {
			test_queue_rcv_msg (qinfo.queue, 4000, -1);
			delay_ms (500);
		}
		pthread_join (sender_test_tid, NULL);
	}
	flush_queue_count = 0;
	CU_ASSERT (libpd_qdestroy (&qinfo.queue, &qfree) == 0);
	CU_ASSERT (flush_queue_count == 0);
}

void wait_auth_received (void)
{
	if (!is_auth_received ()) {
		libpd_log (LEVEL_INFO, ("Waiting for auth received\n"));
		sleep(1);
	}
	if (!is_auth_received ()) {
		libpd_log (LEVEL_INFO, ("Waiting for auth received\n"));
		sleep(1);
	}
	CU_ASSERT (is_auth_received ());
}

void test_send_only (void)
{
	unsigned event_num = 0;
	libpd_cfg_t cfg1 = {.service_name = service_name1,
		.receive = false, .keepalive_timeout_secs = 0};
	libpd_cfg_t cfg2 = {.service_name = service_name2,
		.receive = false, .keepalive_timeout_secs = 0};
	CU_ASSERT (libparodus_init(&test_instance1, &cfg1) == 0);
	CU_ASSERT (libparodus_init(&test_instance2, &cfg2) == 0);
	CU_ASSERT (send_event_msgs (NULL, &event_num, 200, true) == 0);
	CU_ASSERT (libparodus_shutdown (&test_instance1) == 0);
	CU_ASSERT (libparodus_shutdown (&test_instance2) == 0);

	cfg1.test_flags |= CFG_TEST_CONNECT_ON_EVERY_SEND;
	cfg2.test_flags |= CFG_TEST_CONNECT_ON_EVERY_SEND;
	CU_ASSERT (libparodus_init(&test_instance1, &cfg1) == 0);
	CU_ASSERT (libparodus_init(&test_instance2, &cfg2) == 0);
	CU_ASSERT (send_event_msgs (NULL, &event_num, 200, true) == 0);
	CU_ASSERT (libparodus_shutdown (&test_instance1) == 0);
	CU_ASSERT (libparodus_shutdown (&test_instance2) == 0);

}

void test_multiple_inits (void)
{
	#define NUM_INSTANCES 1000
	int i, rtn, shutdown_ct;
	extra_err_info_t err_info;
	libpd_cfg_t cfg1 = {.service_name = service_name1,
		.receive = false, .keepalive_timeout_secs = 0,
		.parodus_url = GOOD_PARODUS_URL, .client_url = GOOD_CLIENT_URL};
	libpd_instance_t instance_table[NUM_INSTANCES];

	libpd_log (LEVEL_INFO, ("LIBPD_TEST: Test many instances\n"));
	for (i=0; i<NUM_INSTANCES; i++) {
		rtn = libparodus_init_dbg(&instance_table[i], &cfg1, &err_info);
		if (rtn != 0)
			break;
	}
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: started %d instances\n", i));
	shutdown_ct = 0;
	if (rtn != 0) {
		libpd_log (LEVEL_INFO, 
			("LIBPD_TEST: init failed with code %d after %d instances\n", rtn, i));
		CU_ASSERT (rtn == LIBPD_ERROR_INIT_CONNECT);
		if (rtn != LIBPD_ERROR_INIT_INST) {
			libpd_log_err (LEVEL_INFO, err_info.oserr,
				("LIBPD_TEST: init err (%x)\n", -err_info.err_detail));
			CU_ASSERT (err_info.err_detail == LIBPD_ERR_INIT_SEND_CREATE);
			rtn = libparodus_shutdown (&instance_table[i]);
			if (rtn == 0)
				shutdown_ct++;
		}
	}
	--i;
	rtn = 0;
	while (i >= 0) {
		rtn = libparodus_shutdown (&instance_table[i]);
		if (rtn != 0)
			break;
		shutdown_ct++;
		--i;
	}
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: shut down %d instances\n", shutdown_ct));
	CU_ASSERT (rtn == 0);
}

static void *SecondReceiverTask()
{
  int rtn;
  wrp_msg_t *wrp_msg;
  unsigned reply_error_count = 0;
  unsigned msgs_received_count = 0;

	libpd_log (LEVEL_INFO, ("LIBPD_TEST: starting Rcvr 2 msg receive loop\n"));
	while (true) {
		rtn = libparodus_receive (test_instance1, &wrp_msg, 2000);
		if (rtn == 1) {
			libpd_log (LEVEL_INFO, ("LIBPD_TEST: Rcvr 2 Timed out waiting for msg\n"));
			continue;
		}
		if (rtn != 0)
			break;
		show_wrp_msg (wrp_msg, 2);
		if (wrp_msg->msg_type == WRP_MSG_TYPE__REQ)
			if (send_reply (test_instance1, wrp_msg) != 0)
				reply_error_count++;
		wrp_free_struct (wrp_msg);
		msgs_received_count++;
	}
	CU_ASSERT (reply_error_count == 0);
	if (reply_error_count != 0) {
		libpd_log (LEVEL_INFO, ("LIBPD_TEST: Rcvr 2 Reply Error Count %u\n", reply_error_count));
	}
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: Rcvr 2 Messages received %u\n", msgs_received_count));
	libparodus_close_receiver (test_instance1);
	return NULL;
}

void test_1(void)
{
	unsigned msgs_received_count = 0;
	unsigned timeout_cnt = 0;
	unsigned reply_error_count = 0;
	int reconnect_count, keep_alive_count;
	int rtn, oserr;
	int test_sock, dup_sock;
	libpd_mq_t test_queue;
	extra_err_info_t err_info;
	wrp_msg_t *wrp_msg;
	unsigned event_num = 0;
	unsigned msg_num = 0;
	libpd_instance_t current_instance;
	libpd_instance_t null_instance = NULL;
	libpd_cfg_t cfg1 = {.service_name = service_name1,
		.receive = true, .keepalive_timeout_secs = 0};
	libpd_cfg_t cfg2 = {.service_name = service_name2,
		.receive = true, .keepalive_timeout_secs = 0};

	test_time ();

	CU_ASSERT_FATAL (check_current_dir() == 0);

	test_queues ();

	//test_set_cfg (&cfg);
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: test connect receiver, good IP\n"));
	test_sock = connect_receiver (TEST_RCV_URL, 20, &oserr);
	CU_ASSERT (test_sock >= 0) ;
	if (test_sock >= 0)
		shutdown_socket(&test_sock);
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: test connect receiver, bad IP\n"));
	test_sock = connect_receiver (BAD_RCV_URL, 0, &oserr);
	CU_ASSERT (test_sock < 0);
	CU_ASSERT (oserr == EINVAL);
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: test connect receiver, good IP\n"));
	test_sock = connect_receiver (TEST_RCV_URL, 20, &oserr);
	CU_ASSERT (test_sock >= 0) ;
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: test connect duplicate receiver\n"));
	dup_sock = connect_receiver (TEST_RCV_URL, 20, &oserr);
	CU_ASSERT (dup_sock < 0);
	CU_ASSERT (oserr == EADDRINUSE);
	if (test_sock >= 0)
		shutdown_socket(&test_sock);
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: test connect sender, good IP\n"));
	test_sock = connect_sender (TEST_SEND_URL, &oserr);
	CU_ASSERT (test_sock >= 0) ;
	if (test_sock >= 0)
		shutdown_socket(&test_sock);
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: test connect sender, bad IP\n"));
	test_sock = connect_sender (BAD_SEND_URL, &oserr);
	CU_ASSERT (test_sock < 0);
	CU_ASSERT (oserr == EINVAL);
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: test create wrp queue\n"));
	CU_ASSERT (test_create_wrp_queue (&test_queue, "/TEST_QUEUE", &oserr) == 0);
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: test libparodus receive good\n"));
	test_send_wrp_queue_ok (test_queue, &oserr);
	CU_ASSERT (libparodus_receive__ (test_queue, &wrp_msg, 500, &oserr) == 0);
	wrp_free_struct (wrp_msg);
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: test wrp_flush_queue\n"));
	test_send_wrp_queue_ok (test_queue, &oserr);
	test_send_wrp_queue_ok (test_queue, &oserr);
	test_send_wrp_queue_ok (test_queue, &oserr);
	CU_ASSERT (flush_wrp_queue (test_queue, 500, &oserr) == 3);
	CU_ASSERT (flush_wrp_queue (test_queue, 500, &oserr) == 0);
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: test wrp_flush_queue with close msg\n"));
	test_send_wrp_queue_ok (test_queue, &oserr);
	test_send_wrp_queue_ok (test_queue, &oserr);
	test_close_receiver (test_queue, &oserr);
	CU_ASSERT (flush_wrp_queue (test_queue, 500, &oserr) == 3);

	libpd_log (LEVEL_INFO, ("LIBPD_TEST: test libparodus receive timeout\n"));
	CU_ASSERT (libparodus_receive__ (test_queue, &wrp_msg, 500, &oserr) == 1);
	CU_ASSERT (test_close_receiver (test_queue, &oserr) == 0);
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: test libparodus receive close msg\n"));
	rtn = libparodus_receive__ (test_queue, &wrp_msg, 500, &oserr);
	if (rtn != 2) {
		libpd_log (LEVEL_INFO, ("LIBPD_TEST: expected receive rtn==2 after close, got %d\n", rtn));
	}
	CU_ASSERT (rtn == 2);

	test_close_wrp_queue (&test_queue); 

	CU_ASSERT (libparodus_receive__ 
		(test_queue, &wrp_msg, 500, &oserr) == LIBPD_ERR_RCV_QUEUE_NULL);

	rtn = libparodus_receive (null_instance, &wrp_msg, 500);
	CU_ASSERT (rtn == LIBPD_ERROR_RCV_NULL_INST);
  CU_ASSERT (strcmp (libparodus_strerror (rtn), 
			"Error on libparodus receive. Null instance given.") == 0);
	rtn = libparodus_close_receiver (null_instance);
	CU_ASSERT (rtn == LIBPD_ERROR_CLOSE_RCV_NULL_INST);
  CU_ASSERT (strcmp (libparodus_strerror (rtn), 
			"Error on libparodus close receiver. Null instance given.") == 0);
	rtn = libparodus_send (null_instance, wrp_msg);
	CU_ASSERT (rtn == LIBPD_ERROR_SEND_NULL_INST);
  CU_ASSERT (strcmp (libparodus_strerror (rtn), 
			"Error on libparodus send. Null instance given.") == 0);
	
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: libparodus_init bad parodus ip\n"));
	cfg1.receive = true;
	cfg1.parodus_url = BAD_PARODUS_URL;
	CU_ASSERT (libparodus_init_dbg (&test_instance1, &cfg1, &err_info) == LIBPD_ERROR_INIT_CFG);
        libpd_log (LEVEL_INFO, ("LIBPD_TEST: rtn %x, oserr %d\n", 
            err_info.errdetail, err_info.oserr));
	CU_ASSERT (err_info.err_detail == LIBPD_ERR_INIT_SEND_CONN);
	CU_ASSERT (err_info.oserr == EINVAL);
	CU_ASSERT (libparodus_shutdown (&test_instance1) == 0);
	cfg1.parodus_url = GOOD_PARODUS_URL;
	cfg1.client_url = BAD_CLIENT_URL;
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: libparodus_init bad client url\n"));
	CU_ASSERT (libparodus_init (&test_instance1, &cfg1) == LIBPD_ERROR_INIT_CFG);
	//CU_ASSERT (exterr == EINVAL);
	CU_ASSERT (libparodus_shutdown (&test_instance1) == 0);
	cfg1.client_url = GOOD_CLIENT_URL;
	//cfg1.service_name = "VeryVeryVeryVeryVeryVeryVeryVeryVeryVeryVeryVeryLongService";
	//libpd_log (LEVEL_INFO, ("LIBPD_TEST: libparodus_init service name too long\n"));
	//CU_ASSERT (libparodus_init (&test_instance1, &cfg1) == LIBPD_ERROR_INIT_INST);
	//CU_ASSERT (libparodus_shutdown (&test_instance1) == 0);
	//cfg1.service_name = service_name1;

	if (connect_on_every_send) {
		cfg1.test_flags |= CFG_TEST_CONNECT_ON_EVERY_SEND;
		cfg2.test_flags |= CFG_TEST_CONNECT_ON_EVERY_SEND;
	}

	if (no_mock_send_only_test) {
		test_send_only ();
		return;
	}

	if (using_mock) {
		rtn = create_end_pipe ();
		CU_ASSERT_FATAL (rtn == 0);
		if (rtn != 0)
			return;
		if (start_mock_parodus () == 0)
			return; // if in child process
		libpd_log (LEVEL_INFO, ("LIBPD mock_parodus started\n"));
		rtn = open_end_pipe ();
	}

	libpd_log (LEVEL_INFO, ("LIBPD_TEST: no receive option\n"));
	cfg1.receive = false;
	CU_ASSERT (libparodus_init(&test_instance1, &cfg1) == 0);
	CU_ASSERT (send_event_msgs (NULL, &event_num, 5, false) == 0);
	CU_ASSERT (libparodus_receive 
		(test_instance1, &wrp_msg, 500) == LIBPD_ERROR_RCV_CFG);
	if (do_send_blocking_test)
		test_send_blocking ();
	else if (do_send_disconnect_test)
		test_send_disconnect ();
	CU_ASSERT (libparodus_shutdown (&test_instance1) == 0);

	if (do_multiple_inits_test)
		test_multiple_inits();  // this test won't work with valgrind

	cfg1.receive = true;
	if (using_mock) {
		cfg1.keepalive_timeout_secs = 20;
	}
	if (switch_service_names) {
		const char *tmp = cfg1.service_name;
		cfg1.service_name = cfg2.service_name;
		cfg2.service_name = tmp;
	}
	rtn = libparodus_init(&test_instance1, &cfg1);
	CU_ASSERT_FATAL (rtn == 0);
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: libparodus_init 1 successful\n"));
	initEndKeypressHandler ();

	//wait_auth_received ();
	//if (is_auth_received()) {
		libpd_log (LEVEL_INFO, ("LIBPD_TEST: Test invalid wrp message\n"));
		wrp_msg = (wrp_msg_t *) "*** Invalid WRP message\n";
		CU_ASSERT (libparodus_send 
			(test_instance1, wrp_msg) == LIBPD_ERROR_SEND_WRP_MSG);
	//}
	current_instance = test_instance1;
	if (do_multiple_inst_test)  {
		cfg2.receive = true;
		cfg2.client_url = GOOD_CLIENT_URL2;
		rtn = libparodus_init(&test_instance2, &cfg2);
		CU_ASSERT_FATAL (rtn == 0);
		libpd_log (LEVEL_INFO, ("LIBPD_TEST: libparodus_init 2 successful\n"));
		current_instance = test_instance2;
	}
	if (do_multiple_rcv_test) {
		int err = pthread_create(&SecondReceiverThreadId, NULL, 
			SecondReceiverTask, NULL);
		if (err != 0) 
		{
		  libpd_log_err (LEVEL_ERROR, err, 
			("Error creating second receiver thread\n"));
		  do_multiple_rcv_test = false;
		}
	}
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: starting Rcvr 1 msg receive loop\n"));
	reply_error_count = 0;
	while (true) {
		if (do_multiple_inst_test) {
			if (current_instance == test_instance1)
				current_instance = test_instance2;
			else
				current_instance = test_instance1;
		}
		rtn = libparodus_receive (current_instance, &wrp_msg, 2000);
		if (rtn == 1) {
			libpd_log (LEVEL_INFO, ("LIBPD_TEST: Rcvr 1 Timed out waiting for msg\n"));
			if (current_instance != test_instance1)
				continue;
#ifdef MOCK_MSG_COUNT
			if (using_mock && ((msgs_received_count+1) >= MOCK_MSG_COUNT)) {
				timeout_cnt++;
				if (timeout_cnt >= 6) {
					libparodus_close_receiver (test_instance1);
					continue;
				}
			}
#endif
			test_get_counts (test_instance1, &keep_alive_count, &reconnect_count);
			if ((reconnect_count == 0) && (msgs_received_count > 0))
				if (send_event_msgs (&msg_num, &event_num, 5, false) != 0)
					break;
			continue;
		}
		if (rtn != 0)
			break;
		show_wrp_msg (wrp_msg, 1);
		if (wrp_msg->msg_type == WRP_MSG_TYPE__REQ)
			if (send_reply (current_instance, wrp_msg) != 0)
				reply_error_count++;
		wrp_free_struct (wrp_msg);
		if (current_instance == test_instance1) {
			timeout_cnt = 0;
			msgs_received_count++;
			if (send_event_msgs (&msg_num, &event_num, 5, false) != 0)
				break;
		}
	}
	CU_ASSERT (reply_error_count == 0);
	if (reply_error_count != 0) {
		libpd_log (LEVEL_INFO, ("LIBPD_TEST: Rcvr 1 Reply Error Count %u\n", reply_error_count));
	}
	if (using_mock) {
		libpd_log (LEVEL_INFO, ("Keep alive msgs received %d\n", keep_alive_count));
		test_get_counts (test_instance1, &keep_alive_count, &reconnect_count);
		CU_ASSERT (keep_alive_count == NUM_KEEP_ALIVE_MSGS);
		CU_ASSERT (reconnect_count == 1);
#ifdef MOCK_MSG_COUNT
		bool close_pipe = (msgs_received_count < MOCK_MSG_COUNT);
#else
		bool close_pipe = true;
#endif
		if (close_pipe) {
			libpd_log (LEVEL_INFO, ("LIBPD writing end pipe\n"));
			write_end_pipe (); 
			libpd_log (LEVEL_INFO, ("LIBPD closing end pipe\n"));
			close (end_pipe_fd);
			wait (NULL);
		}
	} else if (do_multiple_rcv_test) {
		libparodus_close_receiver (test_instance1);
		pthread_join (SecondReceiverThreadId, NULL);
	}
	libpd_log (LEVEL_INFO, ("LIBPD_TEST: Rcvr 1 Messages received %u\n", msgs_received_count));
	CU_ASSERT (libparodus_shutdown (&test_instance1) == 0);
	if (do_multiple_inst_test) {
		CU_ASSERT (libparodus_shutdown (&test_instance2) == 0);
	}
}

/*
 * @brief To initiate end keypress handler
 */
static void initEndKeypressHandler()
{
	int err = 0;
	err = pthread_create(&endKeypressThreadId, NULL, endKeypressHandlerTask, NULL);
	if (err != 0) 
	{
		libpd_log_err (LEVEL_ERROR, err, ("Error creating End Keypress Handler thread\n"));
	}
	else 
	{
		printf ("End Keypress handler Thread created successfully\n");
		printf ("\n--->> Press <Enter> to shutdown the test. ---\n");
	}
}

/*
 * @brief To handle End Keypress
 */
static void *endKeypressHandlerTask()
{
	char inbuf[10];
	memset(inbuf, 0, 10);
	while (true) {
		fgets (inbuf, 10, stdin);
		if ((inbuf[0] != '\n') && (inbuf[0] != '\0')) {
            printf ("endKeyPressHandler exiting\n");
			break;
		}
	}
	libparodus_close_receiver (test_instance1);
	return NULL;
}


void add_suites( CU_pSuite *suite )
{
    *suite = CU_add_suite( "libparodus tests", NULL, NULL );
    CU_add_test( *suite, "Test 1", test_1 );
}

/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/
int main( int argc, char **argv __attribute__((unused)) )
{
    unsigned rv = 1;
    CU_pSuite suite = NULL;

		if (argc <= 1)
			using_mock = true;
		else {
			const char *arg = argv[1];
			if ((arg[0] == 's') || (arg[0] == 'S'))
				no_mock_send_only_test = true;
			if ((arg[0] == 'r') || (arg[0] == 'R'))
				do_multiple_rcv_test = true;
			if ((arg[0] == 'm') || (arg[0] == 'M'))
				do_multiple_inst_test = true;
			if ((arg[0] == 'b') || (arg[0] == 'B')) {
				using_mock = true;
				do_send_blocking_test = true;
			}
			if ((arg[0] == 'd') || (arg[0] == 'D')) {
				using_mock = true;
				do_send_disconnect_test = true;
			}
			if ((arg[0] == 'i') || (arg[0] == 'I')) {
				using_mock = true;
				do_multiple_inits_test = true;
			}
			if ((arg[0] == 'c') || (arg[0] == 'C')) {
				using_mock = true;
				connect_on_every_send = true;
			}
			if (arg[0] == '2') {
				switch_service_names = true;
			}
		
		}

    if( CUE_SUCCESS == CU_initialize_registry() ) {
        add_suites( &suite );

        if( NULL != suite ) {
            CU_basic_set_mode( CU_BRM_VERBOSE );
            CU_basic_run_tests();
            printf( "\n" );
            CU_basic_show_failures( CU_get_failure_list() );
            printf( "\n\n" );
            rv = CU_get_number_of_tests_failed();
        }

        CU_cleanup_registry();
    }

    if( 0 != rv ) {
        return 1;
    }
    return 0;
}
