#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/types.h>
#include <linux/i2c-dev.h>
#include <pthread.h>
#include <RF24/RF24.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

//**********************************************************************
//User Values
//**********************************************************************
#define PAYLOAD_SIZE 3
#define SOCKET_PAYLOAD_SIZE 50

#define BUZZERWRITE_GPIO 20

//**********************************************************************
//Constants
//**********************************************************************
#define IN 0
#define OUT 1

#define TRANSMITTER 0
#define RECEIVER 1
#define DB 2

#define ACK 0
#define DATA 1
#define RESEND 2

uint64_t address[3] = {0x7878787878LL,
                        0xB3B4B5B6F1LL,
                        0xB3B4B5B6CDLL};



//**********************************************************************
//Variables
//**********************************************************************
long unsigned int waiting_time = 10;
int exit_marker = 0;

//**********************************************************************
//RF
//**********************************************************************
RF24 radio(22, 0);

//**********************************************************************
//Socket
//**********************************************************************
int sock;
struct sockaddr_in serv_addr;
char server_ip[20] = "192.168.";
int server_port;

//**********************************************************************
//LCD
//**********************************************************************
int lcd_status = 0;
char * username;
int rc;

//**********************************************************************
//Buzzer
//**********************************************************************
int buzzer_status = 0;

//**********************************************************************
//Payload
//**********************************************************************
struct PayloadStruct {
    int message[PAYLOAD_SIZE];
    uint8_t counter;
};
PayloadStruct payload;
char socket_payload[SOCKET_PAYLOAD_SIZE];

//**********************************************************************
//Time
//**********************************************************************
time_t ct;
struct tm tm;

//**********************************************************************
//GPIO
//**********************************************************************
#define GPIO_VALUE_MAX 40
#define GPIO_BUFFER_MAX 3
#define GPIO_DIRECTION_MAX 35

static int
GPIOExport(int pin){

    char buffer[GPIO_BUFFER_MAX];
    ssize_t bytes_written;
    int fd;

    fd = open("/sys/class/gpio/export", O_WRONLY);
    if (-1 == fd){
        fprintf(stderr, "Failed to open export for writing!\n");
        return(-1);
    }

    bytes_written = snprintf(buffer, GPIO_BUFFER_MAX, "%d", pin);
    write(fd, buffer, bytes_written);
    close(fd);
    return(0);
}

static int
GPIOUnexport(int pin){

    char buffer[GPIO_BUFFER_MAX];
    ssize_t bytes_written;
    int fd;

    fd = open("/sys/class/gpio/unexport", O_WRONLY);
    if (-1 == fd){
        fprintf(stderr, "Failed to open unexport for writing!\n");
        return(-1);
    }

    bytes_written = snprintf(buffer, GPIO_BUFFER_MAX, "%d", pin);
    write(fd, buffer, bytes_written);
    close(fd);
    return(0);
}

static int
GPIODirection(int pin, int dir){
    static const char s_directions_str[] = "in\0out";

    char path[GPIO_DIRECTION_MAX] = "/sys/class/gpio/gpio%d/direction";
    int fd;

    snprintf(path, GPIO_DIRECTION_MAX, "/sys/class/gpio/gpio%d/direction", pin);

    fd = open(path, O_WRONLY);
    if (-1 == fd){
        fprintf(stderr, "Failed to open gpio direction for writing!\n");
        return(-1);
    }

    if (-1 == write(fd, &s_directions_str[IN == dir ? 0:3], IN == dir ? 2 : 3)){
        fprintf(stderr, "Failed to set direction!\n");
        close(fd);
        return(-1);
    }

    close(fd);
    return(0);

}

static int
GPIOWrite(int pin, int value){
    static const char s_values_str[] = "01";

    char path[GPIO_VALUE_MAX];
    int fd;

    snprintf(path, GPIO_VALUE_MAX, "/sys/class/gpio/gpio%d/value", pin);
    fd = open(path, O_WRONLY);
    if (-1 == fd) {
        fprintf(stderr, "Failed to open gpio value for writing!\n");
        return(-1);
    }

    if (1 != write(fd, &s_values_str[LOW == value ? 0 : 1], 1)){
        fprintf(stderr, "Failed to write value!\n");
        close(fd);
        return(-1);
    }

    close(fd);
    return(0);

}

//**********************************************************************
//LCD
//**********************************************************************
// The LCD controller is wired to the I2C port expander with the upper 4 bits
// (D4-D7) connected to the data lines and the lower 4 bits (D0-D3) used as
// control lines. Here are the control line definitions:
//
// Command (0) / Data (1) (aka RS) (D0)
// R/W                             (D1)
// Enable/CLK                      (D2)
// Backlight control               (D3)
//
// The data must be manually clocked into the LCD controller by toggling
// the CLK line after the data has been placed on D4-D7
//
// iC2 port with LCD:
// P7 P6 P5 P4 P3 P2 P1 P0
// D7 D6 D5 D4 BL EN RW RS
// D3 D2 D1 D0 Bl EN RW RS

// LCD CONSTANTS
#define PULSE_PERIOD 500
#define CMD_PERIOD 4100
#define BACKLIGHT 8 // 0x00001000
#define DATA 1 //rc

static int iBackLight = BACKLIGHT;
static int file_i2c = -1;
int nowdisplay = -1;

static void WriteCommand(unsigned char ucCMD) {
	unsigned char uc;

	uc = (ucCMD & 0xf0) | iBackLight; // most significant nibble sent first
	write(file_i2c, &uc, 1);
	usleep(PULSE_PERIOD); // manually pulse the clock line
	uc |= 4; // enable pulse
	write(file_i2c, &uc, 1);
	usleep(PULSE_PERIOD);
	uc &= ~4; // toggle pulse
	write(file_i2c, &uc, 1);
	usleep(CMD_PERIOD);
	uc = iBackLight | (ucCMD << 4); // least significant nibble
	write(file_i2c, &uc, 1);
	usleep(PULSE_PERIOD);
        uc |= 4; // enable pulse
        write(file_i2c, &uc, 1);
        usleep(PULSE_PERIOD);
        uc &= ~4; // toggle pulse
        write(file_i2c, &uc, 1);
	usleep(CMD_PERIOD);

}

int lcd1602Control(int bBacklight, int bCursor, int bBlink) {
unsigned char ucCMD = 0xc; // display control

	if (file_i2c < 0)
		return 1;
	iBackLight = (bBacklight) ? BACKLIGHT : 0;
	if (bCursor)
		ucCMD |= 2;
	if (bBlink)
		ucCMD |= 1;
	WriteCommand(ucCMD);

	return 0;
}

int lcd1602WriteString(const char *text) {
unsigned char ucTemp[2];
int i = 0;

	if (file_i2c < 0 || text == NULL)
		return 1;

	while (i<16 && *text)
	{
		ucTemp[0] = iBackLight | DATA | (*text & 0xf0);
		write(file_i2c, ucTemp, 1);
		usleep(PULSE_PERIOD);
		ucTemp[0] |= 4; // pulse E
		write(file_i2c, ucTemp, 1);
		usleep(PULSE_PERIOD);
		ucTemp[0] &= ~4;
		write(file_i2c, ucTemp, 1);
		usleep(PULSE_PERIOD);
		ucTemp[0] = iBackLight | DATA | (*text << 4);
		write(file_i2c, ucTemp, 1);
		ucTemp[0] |= 4; // pulse E
                write(file_i2c, ucTemp, 1);
                usleep(PULSE_PERIOD);
                ucTemp[0] &= ~4;
                write(file_i2c, ucTemp, 1);
                usleep(CMD_PERIOD);
		text++;
		i++;
	}
	return 0;
}

int lcd1602Clear(void) {
	if (file_i2c < 0)
		return 1;
	WriteCommand(0x0E);
	return 0;

}

int lcd1602Init(int iChannel, int iAddr) {
	char szFile[32];
	int rc;

	sprintf(szFile, "/dev/i2c-%d", iChannel);
	file_i2c = open(szFile, O_RDWR);
	if (file_i2c < 0)
	{
		fprintf(stderr, "Error opening i2c device; not running as sudo?\n");
		return 1;
	}
	rc = ioctl(file_i2c, I2C_SLAVE, iAddr);
	if (rc < 0)
	{
		close(file_i2c);
		fprintf(stderr, "Error setting I2C device address\n");
		return 1;
	}
	iBackLight = BACKLIGHT; // turn on backlight
	WriteCommand(0x02); // Set 4-bit mode of the LCD controller
	WriteCommand(0x28); // 2 lines, 5x8 dot matrix
	WriteCommand(0x0c); // display on, cursor off
	WriteCommand(0x06); // inc cursor to right when writing and don't scroll
	WriteCommand(0x80); // set cursor to row 1, column 1
	lcd1602Clear();	    // clear the memory

	return 0;
}

int lcd1602SetCursor(int x, int y) {
unsigned char cCmd;

	if (file_i2c < 0 || x < 0 || x > 15 || y < 0 || y > 1)
		return 1;

	cCmd = (y==0) ? 0x80 : 0xc0;
	cCmd |= x;
	WriteCommand(cCmd);
	return 0;

}

void lcd1602Shutdown(void) {
	iBackLight = 0; // turn off backlight
	WriteCommand(0x08); // turn off display, cursor and blink
	close(file_i2c);
	file_i2c = -1;
}

void lcd1602ClearScreen() {
    lcd1602SetCursor(0,0);
	lcd1602WriteString("                ");
    lcd1602SetCursor(0,1);
    lcd1602WriteString("                ");
    lcd1602SetCursor(0,0);
}

//**********************************************************************
//Receiver_Logger / Receiver_Logprinter / Receiver_Logremover
//**********************************************************************
#define LOG_BUFFER_MAX 256
const char * logfilepath = "receiver.txt";

int receiver_logger(const char * message) {
    int fd = open(logfilepath, O_WRONLY|O_CREAT|O_APPEND, 0777);

    if (fd < 0){
        fprintf(stderr, "Logger : File Open Failed\n");
        return -1;
    }

    char str[LOG_BUFFER_MAX];
    sprintf(str, "%02d-%02d-%02d %02d:%02d:%02d : ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    strcat(str, message);
    strcat(str, "\n");
    write(fd, str, strlen(str));
    close(fd);

    return 0;
}

int receiver_logprinter() {
    int fd = open(logfilepath, O_RDONLY, 0777);

    if (fd < 0){
        fprintf(stderr, "Logprinter : File Open Failed\n");
        return -1;
    }

    char buffer;
    int rd;
    while((rd = read(fd, &buffer, sizeof(buffer)))>0){
        fprintf(stderr, "%c", buffer);
    }

    close(fd);

    return 0;
}

int receiver_logremover() {
    remove(logfilepath);
    int fd = open(logfilepath, O_WRONLY|O_CREAT, 0777);

    if (fd < 0){
        fprintf(stderr, "Logremover : File Open Failed\n");
        return -1;
    }

    close(fd);

    return 0;
}

//**********************************************************************
//Receiver_Work
//**********************************************************************
void *receiver_work(void *) {
    while(1) {
        unsigned long start_time = clock();
        //RF Receive
        receiver_logger("Receive Start");
        PayloadStruct received_payload;
        uint8_t pipe;
        bool sended, received;
        int socket_sended, socket_received;
        radio.startListening();
        received = radio.available(&pipe);
        while (!received) {
            if (exit_marker){
                receiver_logger("Work Shutdown");
                return NULL;
            }
            usleep(1000);
            received = radio.available(&pipe);
        }
        radio.stopListening();

        if (received) {
            receiver_logger("Receive Success");
            payload.message[0] = RECEIVER;
            payload.message[1] = ACK;

            radio.read(&received_payload, sizeof(received_payload));
            if(received_payload.message[0] != TRANSMITTER) {
                receiver_logger("Data Not From Transmitter");
                payload.message[1] = RESEND;
            }
            else if(received_payload.message[1] != DATA) {
                receiver_logger("Not DATA");
                payload.message[1] = RESEND;
            }
            else receiver_logger("Sending ACK");

            //RF Send
            sended = radio.write(&payload, sizeof(payload));
            while (!sended) {
                if (((clock() - start_time) / CLOCKS_PER_SEC) > waiting_time) break;
                usleep(1000);
                sended = radio.write(&payload, sizeof(payload));
            }
            if (sended) receiver_logger("Send Success");
            else receiver_logger("Send Failed / Time Out");
        }
        else receiver_logger("Receive Failed / Time Out");

        if(sended && received){
            int sendloop = 1;
            lcd_status = 1;
            buzzer_status = 1;
            while(sendloop){
                //Socket Send
                start_time = clock();
                memset(socket_payload, 0, SOCKET_PAYLOAD_SIZE);
                strcat(socket_payload, "RECEIVER DATA ");
                char temp[20];
                sprintf(temp, "%d", received_payload.message[2]);
                strcat(socket_payload, temp);
                socket_sended = send(sock, socket_payload, SOCKET_PAYLOAD_SIZE, 0);
                if(socket_sended){
                    //Socket Receive
                    receiver_logger("Socket Send Success");

                    memset(socket_payload, 0, SOCKET_PAYLOAD_SIZE);
                    socket_received = recv(sock, socket_payload, SOCKET_PAYLOAD_SIZE, 0);
                    if (socket_received){
                        receiver_logger("Socket Receive Success");
                        char * parsed = strtok(socket_payload, " ");
                        if(parsed != NULL && strcmp(parsed, "DB")==0){
                            parsed = strtok(NULL, " ");
                            if(parsed != NULL && strcmp(parsed, "DATA")==0){
                                parsed = strtok(NULL, " ");
                                if(parsed != NULL){
                                    username = parsed;
                                    parsed = strtok(NULL, " ");
                                    if(parsed != NULL && atoi(parsed) >= 0){
                                        sendloop = 0;
                                        if(atoi(parsed) == 0){
                                            receiver_logger("Accepted User");
                                            lcd_status = 2;
                                            buzzer_status = 2;
                                        }
                                        else if(atoi(parsed) == 1){
                                            receiver_logger("Rejected User");
                                            lcd_status = 3;
                                            buzzer_status = 3;
                                        }
                                        else if(atoi(parsed) == 2){
                                            receiver_logger("User Not In List");
                                            lcd_status = 4;
                                            buzzer_status = 4;
                                        }
                                    }
                                    else receiver_logger("Wrong Data");
                                }
                                else receiver_logger("Wrong Data");
                            }
                            else receiver_logger("Not DATA");
                        }
                        else receiver_logger("Data Not From DB");
                    }
                    else receiver_logger("Socket Receive Failed / Time Out");
                }
                else receiver_logger("Socket Send Failed / Time Out");
            }
        }

        usleep(1000);
    }
}

//**********************************************************************
//Receiver_LCDcontroller
//**********************************************************************

void *receiver_lcdcontroller(void *) {
    while(1) {
        lcd1602ClearScreen();
        switch (lcd_status){
            case 0: // WAITING
                lcd1602WriteString("Waiting");
                while(lcd_status == 0) {
                    if(exit_marker){
                        lcd1602Shutdown();
                        receiver_logger("LCDcontroller Shutdown");
                        return NULL;
                    }
                    usleep(1000);
                }
            break;
            case 1: // RECEIVING
                lcd1602WriteString("Receiving From DB");
            break;
            case 2: // ACCEPTED
                lcd1602WriteString("Accepted");
                lcd1602SetCursor(0,1);
                lcd1602WriteString(username);
                sleep(2);
                lcd_status = 0;
            break;
            case 3: // REJECTED
                lcd1602WriteString("Rejected");
                lcd1602SetCursor(0,1);
                lcd1602WriteString(username);
                sleep(2);
                lcd_status = 0;
            break;
            case 4: // NOT IN DB
                lcd1602WriteString("Not In DB");
                sleep(2);
                lcd_status = 0;
            break;
        }
        usleep(1000);
    }
}


//**********************************************************************
//Receiver_Buzzercontroller
//**********************************************************************
void *receiver_buzzercontroller(void *){
    while(1) {
        switch (buzzer_status){
            case 0: // WAITING
            case 1: // RECEIVING
                GPIOWrite(BUZZERWRITE_GPIO, 1);
                while(buzzer_status == 0 || buzzer_status == 1){
                    if(exit_marker){
                        GPIOUnexport(BUZZERWRITE_GPIO);
                        receiver_logger("Buzzercontroller Shutdown");
                        return NULL;
                    }
                    usleep(1000);
                }
            break;
            case 2: // ACCEPTED
                GPIOWrite(BUZZERWRITE_GPIO, 0);
                sleep(1);
                GPIOWrite(BUZZERWRITE_GPIO, 1);
                usleep(100000);
                GPIOWrite(BUZZERWRITE_GPIO, 0);
                sleep(1);
                GPIOWrite(BUZZERWRITE_GPIO, 1);
                usleep(50000);
                GPIOWrite(BUZZERWRITE_GPIO, 0);
                sleep(1);
                GPIOWrite(BUZZERWRITE_GPIO, 1);
                buzzer_status = 0;
            break;
            case 3: // REJECTED
                GPIOWrite(BUZZERWRITE_GPIO, 0);
                sleep(1);
                GPIOWrite(BUZZERWRITE_GPIO, 1);
                usleep(50000);
                GPIOWrite(BUZZERWRITE_GPIO, 0);
                sleep(1);
                GPIOWrite(BUZZERWRITE_GPIO, 1);
                usleep(50000);
                GPIOWrite(BUZZERWRITE_GPIO, 0);
                sleep(1);
                GPIOWrite(BUZZERWRITE_GPIO, 1);
                usleep(50000);
                GPIOWrite(BUZZERWRITE_GPIO, 0);
                sleep(1);
                GPIOWrite(BUZZERWRITE_GPIO, 1);
                buzzer_status = 0;
            break;
            case 4: // NOT IN DB
                GPIOWrite(BUZZERWRITE_GPIO, 0);
                sleep(1);
                GPIOWrite(BUZZERWRITE_GPIO, 1);
                usleep(100000);
                GPIOWrite(BUZZERWRITE_GPIO, 0);
                sleep(1);
                GPIOWrite(BUZZERWRITE_GPIO, 1);
                buzzer_status = 0;
            break;
        }
        usleep(1000);
    }
}

//**********************************************************************
//Receiver_CLI
//**********************************************************************
#define CLI_BUFFER_MAX 256

void *receiver_cli(void *){
    char input[CLI_BUFFER_MAX];
    while(1){
        fprintf(stderr, "Receiver $ ");
        scanf("%[^\n]", input);
        if(input[0] == '\0') continue;
        char * parsed = strtok(input, " ");
        if(strcmp(parsed, "log") == 0) {
            parsed = strtok(NULL, " ");
            if(parsed == NULL || strcmp(parsed, "-p") == 0) receiver_logprinter();
            else if(strcmp(parsed, "-r") == 0) {
                fprintf(stderr, "Removing Log\n");
                receiver_logremover();
            }
            else fprintf(stderr, "Unknown Parameter %s\n", parsed);
        }
        else if (strcmp(parsed, "time") == 0){
            parsed = strtok(NULL, " ");
            if(parsed == NULL || strcmp(parsed, "-p") == 0) fprintf(stderr, "Waiting Time is %ld\n", waiting_time);
            else if(strcmp(parsed, "-c") == 0) {
                parsed = strtok(NULL, " ");
                if (parsed == NULL) fprintf(stderr, "Usage : time -c <VALUE>\n\n");
                else if(atoi(parsed) > 0) {
                    waiting_time = atoi(parsed);
                    char str[LOG_BUFFER_MAX];
                    sprintf(str, "Waiting Time Changed to %ld", waiting_time);
                    receiver_logger(str);
                    fprintf(stderr, "Waiting Time Changed to %ld\n", waiting_time);
                }
                else fprintf(stderr, "Invalid Value %s\n", parsed);
            }
            else fprintf(stderr, "Unknown Parameter %s\n", parsed);
        }
        else if (strcmp(parsed, "clear") == 0) system("clear");
        else if (strcmp(parsed, "exit") == 0){
            exit_marker = 1;
            receiver_logger("CLI Shutdown");
            return NULL;
        }
        else fprintf(stderr, "Unknown Command : %s\n", parsed);
        while(getchar() != '\n');
    }
}

//**********************************************************************
//MAIN
//**********************************************************************
int main(int argc, char** argv) {

    //Initialize Time
    ct = time(NULL);
    tm = *localtime(&ct);
    receiver_logger("System Start");

    //Initialize RF
    if (!radio.begin()) {
        receiver_logger("RF Initialization Failed");
        fprintf(stderr, "RF Initialization Failed\n");
        exit(0);
    }
    radio.setPALevel(RF24_PA_LOW);
    radio.setPayloadSize(sizeof(payload));
    radio.openWritingPipe(address[RECEIVER]);
    radio.openReadingPipe(1, address[TRANSMITTER]);

    //Initialize Socket
    sock = socket(PF_INET, SOCK_STREAM, 0);
    if(sock == -1){
        receiver_logger("Socket Initialization Failed");
        fprintf(stderr, "Socket Initialization Failed\n");
        exit(0);
    }
    if(argc != 3){
        char temp[10];
        fprintf(stderr, "Enter Server's IP : 192.168.");
        scanf("%s", temp);
        strcat(server_ip, temp);
        fprintf(stderr, "Enter Server's Port : ");
        scanf("%d", &server_port);
        while(getchar() != '\n');
    }
    else{
        strcpy(server_ip, argv[1]);
        server_port = atoi(argv[2]);
    }
    memset(&serv_addr, 0 , sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(server_ip);
    serv_addr.sin_port = htons(server_port);

    char str[LOG_BUFFER_MAX];
    sprintf(str, "Socket Set With IP %s Port %d", server_ip, server_port);
    receiver_logger(str);

    fprintf(stderr, "Connecting to DB...\n");
    while(connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr))==-1){
        usleep(10000);
    }
    fprintf(stderr, "Connected to DB\n");

    //Initialize LCD
    rc = lcd1602Init(1, 0x27); //i2c address inputs
    if (rc) {
        receiver_logger("LCD Initialization Failed");
        printf("LCD Initialization Failed\n");
        exit(0);
    }

    //Initialize Buzzer
    if (-1 == GPIOExport(BUZZERWRITE_GPIO)) {
        receiver_logger("Buzzer GPIO Export Failed");
        printf("Buzzer GPIO Export Failed\n");
        exit(0);
    }
    if(-1 == GPIODirection(BUZZERWRITE_GPIO, OUT)){
        receiver_logger("Buzzer GPIO Direction Setting Failed");
        printf("Buzzer GPIO Direction Setting Failed\n");
        exit(0);
    }

    //Initialize Threads & Work Start
    pthread_t p_thread[4];
    int status;
    if(pthread_create(&p_thread[0], NULL, receiver_work, NULL) < 0){
        fprintf(stderr, "Work Thread Failed\n");
        exit(0);
    }
    if(pthread_create(&p_thread[1], NULL, receiver_cli, NULL) < 0){
        fprintf(stderr, "CLI Thread Failed\n");
        exit(0);
    }
    if(pthread_create(&p_thread[2], NULL, receiver_lcdcontroller, NULL) < 0){
        fprintf(stderr, "LCDcontroller Thread Failed\n");
        exit(0);
    }
    if(pthread_create(&p_thread[3], NULL, receiver_buzzercontroller, NULL) < 0){
        fprintf(stderr, "Buzzercontroller Thread Failed\n");
        exit(0);
    }

    //Work End
    pthread_join(p_thread[0], (void **)&status);
    pthread_join(p_thread[1], (void **)&status);
    pthread_join(p_thread[2], (void **)&status);
    pthread_join(p_thread[3], (void **)&status);

    close(sock);

    receiver_logger("System Shutdown");
    return 0;
} // main
