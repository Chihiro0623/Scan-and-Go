#include <fcntl.h>
#include <pthread.h>
#include <RF24/RF24.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

//**********************************************************************
//User Values
//**********************************************************************
int USER_ID = 12345;
#define PAYLOAD_SIZE 3

#define BUTTONREAD_GPIO 20
#define BUTTONWRITE_GPIO 21

#define LEDWRITE_PWM 0

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
//Payload
//**********************************************************************
struct PayloadStruct {
    int message[PAYLOAD_SIZE];
    uint8_t counter;
};
PayloadStruct payload;

//**********************************************************************
//Button
//**********************************************************************
int button_pressed = 0;

//**********************************************************************
//LED
//**********************************************************************
int led_status = 0;

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

static int
GPIORead(int pin){
    char path[GPIO_VALUE_MAX];
    char value_str[3];
    int fd;

    snprintf(path, GPIO_VALUE_MAX, "/sys/class/gpio/gpio%d/value",pin);
    fd = open(path, O_RDONLY);
    if(-1 == fd){
        fprintf(stderr, "Failed to open gpio value for reading!\n");
        return(-1);
    }
    if(-1 == read(fd, value_str, 3)){
        fprintf(stderr, "Failed to read value!\n");
        close(fd);
        return(-1);
    }

    close(fd);

    return(atoi(value_str));

}

//**********************************************************************
//PWM
//**********************************************************************

#define PWM_VALUE_MAX 256
#define PWM_BUFFER_MAX 3
#define PWM_DIRECTION_MAX 45

static int
PWMExport(int pwmnum)
{
    char buffer[PWM_BUFFER_MAX];
    int bytes_written;
    int fd;

    fd = open("/sys/class/pwm/pwmchip0/export", O_WRONLY);
    if(-1 == fd){
        fprintf(stderr, "Failed to open in export!\n");
        return(-1);
    }
    bytes_written = snprintf(buffer, PWM_BUFFER_MAX, "%d", pwmnum);
    write(fd, buffer, bytes_written);
    close(fd);
    sleep(1);
    return(0);
}

static int
PWMUnexport(int pwmnum)
{
    char buffer[PWM_BUFFER_MAX];
    int bytes_written;
    int fd;

    fd = open("/sys/class/pwm/pwmchip0/unexport", O_WRONLY);
    if(-1 == fd){
        fprintf(stderr, "Failed to open in unexport!\n");
        return(-1);
    }

    bytes_written = snprintf(buffer, PWM_BUFFER_MAX, "%d", pwmnum);
    write(fd, buffer, bytes_written);
    close(fd);
    sleep(1);

    return(0);
}

static int
PWMEnable(int pwmnum)
{
    static const char s_unenable_str[] = "0";
    static const char s_enable_str[] = "1";


    char path[PWM_DIRECTION_MAX];
    int fd;

    snprintf(path, PWM_DIRECTION_MAX, "/sys/class/pwm/pwmchip0/pwm%d/enable", pwmnum);
    fd = open(path, O_WRONLY);
    if(-1 == fd){
        fprintf(stderr, "Failed to open in enable!\n");
        return -1;
    }

    write(fd, s_unenable_str, strlen(s_unenable_str));
    close(fd);

    fd = open(path, O_WRONLY);
    if (-1 == fd){
        fprintf(stderr, "Failed to open in enable!\n");
        return -1;
    }

    write(fd, s_enable_str, strlen(s_enable_str));
    close(fd);
    return(0);

}

static int
PWMWritePeriod(int pwmnum, int value){
    char s_values_str[PWM_VALUE_MAX];
    char path[PWM_VALUE_MAX];
    int fd, byte;

    snprintf(path, PWM_VALUE_MAX, "/sys/class/pwm/pwmchip0/pwm%d/period", pwmnum);
    fd = open(path, O_WRONLY);
    if (-1 == fd){
        fprintf(stderr, "Failed to open in period!\n");
        return(-1);
    }

    byte = snprintf(s_values_str, PWM_VALUE_MAX, "%d", value);

    if (-1 == write(fd, s_values_str, byte)){
        fprintf(stderr, "Failed to write value in period!\n");
        close(fd);
        return(-1);
    }

    close(fd);
    return(0);
}

static int
PWMWriteDutyCycle(int pwmnum, int value){

    char path[PWM_VALUE_MAX];
    char s_values_str[PWM_VALUE_MAX];
    int fd, byte;

    snprintf(path, PWM_VALUE_MAX, "/sys/class/pwm/pwmchip0/pwm%d/duty_cycle", pwmnum);
    fd = open(path, O_WRONLY);
    if (-1 == fd) {
        fprintf(stderr, "Failed to open in duty_cycle!\n");
        return(-1);
    }

    byte = snprintf(s_values_str, PWM_VALUE_MAX, "%d", value);

    if (-1 == write(fd, s_values_str, byte)) {
        fprintf(stderr, "Failed to write value! in duty_cycle\n");
        close(fd);
        return(-1);
    }

    close(fd);
    return(0);
}

//**********************************************************************
//Transmitter_Logger / Transmitter_Logprinter / Transmitter_Logremover
//**********************************************************************
#define LOG_BUFFER_MAX 256
const char * logfilepath = "transmitter.txt";

int transmitter_logger(const char * message) {
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

int transmitter_logprinter() {
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

int transmitter_logremover() {
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
//Transmitter_Work
//**********************************************************************
void *transmitter_work(void *) {
    while(1) {
        led_status = 0;
        while(!button_pressed) {
            if(exit_marker) {
                transmitter_logger("Work Shutdown");
                return NULL;
            }
            usleep(1000);
        }

        transmitter_logger("Send Start");
        int sendloop = 1;
        unsigned long start_time = clock();
        while (sendloop) {
            if (!button_pressed) break;
            //Send
            led_status = 1;
            payload.message[0] = TRANSMITTER;
            payload.message[1] = DATA;
            payload.message[2] = USER_ID;
            bool sended = radio.write(&payload, sizeof(payload));
            while (!sended) {
                if (((clock() - start_time) / CLOCKS_PER_SEC) > waiting_time){
                    sendloop = 0;
                    break;
                }
                usleep(1000);
                sended = radio.write(&payload, sizeof(payload));
            }
            if (sended) {
                transmitter_logger("Send Success");

                //Receive
                led_status = 2;
                uint8_t pipe;
                radio.startListening();
                bool received = radio.available(&pipe);
                while (!received) {
                    if (((clock() - start_time) / CLOCKS_PER_SEC) > waiting_time){
                        sendloop = 0;
                        break;
                    }
                    usleep(1000);
                    received = radio.available(&pipe);
                }
                radio.stopListening();

                if (received) {
                    transmitter_logger("Receive Success");

                    PayloadStruct received_payload;
                    radio.read(&received_payload, sizeof(received_payload));
                    if(received_payload.message[0] != RECEIVER) transmitter_logger("Data Not From Receiver");
                    else if(received_payload.message[1] != ACK) transmitter_logger("Not ACK");
                    else sendloop = 0;
                }
                else {
                    transmitter_logger("Receive Failed / Time Out");
                    sendloop = 0;
                }
            }
            else {
                transmitter_logger("Send Failed / Time Out");
                sendloop = 0;
            }
            usleep(1000);
        }
        while(button_pressed) usleep(1000);
    } // while
}

//**********************************************************************
//Transmitter_Buttonchecker
//**********************************************************************
void *transmitter_buttonchecker(void *){
    while(!exit_marker){
        if (GPIORead(BUTTONREAD_GPIO) == 0) {
            if(button_pressed != 1) transmitter_logger("Button Pressed");
            button_pressed = 1;
        }
        else button_pressed = 0;
        usleep(100000);
    }
    GPIOUnexport(BUTTONREAD_GPIO);
    GPIOUnexport(BUTTONWRITE_GPIO);
    transmitter_logger("Button Checker Shutdown");
    return NULL;
}

//**********************************************************************
//Transmitter_LEDcontroller
//**********************************************************************
void *transmitter_ledcontroller(void *){
    while(!exit_marker){
        if(led_status == 0) PWMWriteDutyCycle(LEDWRITE_PWM, 0);
        else if(led_status == 1) PWMWriteDutyCycle(LEDWRITE_PWM, 1000000);
        else if(led_status == 2) PWMWriteDutyCycle(LEDWRITE_PWM, 10000000);
        usleep(1000);
    }
    PWMUnexport(LEDWRITE_PWM);
    transmitter_logger("LEDController Shutdown");
    return NULL;
}

//**********************************************************************
//Transmitter_CLI
//**********************************************************************
#define CLI_BUFFER_MAX 256

void *transmitter_cli(void *){
    char input[CLI_BUFFER_MAX];
    while(1){
        fprintf(stderr, "Transmitter $ ");
        scanf("%[^\n]", input);
        if(input[0] == '\0') continue;
        char * parsed = strtok(input, " ");
        if(strcmp(parsed, "send") == 0) {
            parsed = strtok(NULL, " ");
            if(parsed == NULL) {
                button_pressed = 1;
                usleep(1000);
                button_pressed = 0;
            }
        }
        else if(strcmp(parsed, "log") == 0) {
            parsed = strtok(NULL, " ");
            if(parsed == NULL || strcmp(parsed, "-p") == 0) transmitter_logprinter();
            else if(strcmp(parsed, "-r") == 0) {
                fprintf(stderr, "Removing Log\n");
                transmitter_logremover();
            }
            else fprintf(stderr, "Unknown Parameter %s\n", parsed);
        }
        else if (strcmp(parsed, "data") == 0){
            parsed = strtok(NULL, " ");
            if(parsed == NULL || strcmp(parsed, "-p") == 0) fprintf(stderr, "Saved Data is %d\n", USER_ID);
            else if(strcmp(parsed, "-c") == 0) {
                parsed = strtok(NULL, " ");
                if (parsed == NULL) fprintf(stderr, "Usage : data -c <VALUE>\n\n");
                else if(atoi(parsed) > 0) {
                    USER_ID = atoi(parsed);
                    char str[LOG_BUFFER_MAX];
                    sprintf(str, "Data Changed to %d", USER_ID);
                    transmitter_logger(str);
                    fprintf(stderr, "Data Changed to %d\n", USER_ID);
                }
                else fprintf(stderr, "Invalid Value %s\n", parsed);
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
                    transmitter_logger(str);
                    fprintf(stderr, "Waiting Time Changed to %ld\n", waiting_time);
                }
                else fprintf(stderr, "Invalid Value %s\n", parsed);
            }
            else fprintf(stderr, "Unknown Parameter %s\n", parsed);
        }
        else if (strcmp(parsed, "clear") == 0) system("clear");
        else if (strcmp(parsed, "exit") == 0){
            exit_marker = 1;
            transmitter_logger("CLI Shutdown");
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
    transmitter_logger("System Start");

    //Initialize RF
    if (!radio.begin()) {
        fprintf(stderr, "RF Initialization Failed\n");
        return 0;
    }
    radio.setPALevel(RF24_PA_LOW);
    radio.setPayloadSize(sizeof(payload));
    radio.openWritingPipe(address[TRANSMITTER]);
    radio.openReadingPipe(1, address[RECEIVER]);

    //Initialize GPIO
    if (-1 == GPIOExport(BUTTONREAD_GPIO) || -1 == GPIOExport(BUTTONWRITE_GPIO)) {
        transmitter_logger("Button GPIO Export Failed");
        printf("Button GPIO Export Failed\n");
        exit(0);
    }
    if (-1 == GPIODirection(BUTTONREAD_GPIO, IN) || -1 == GPIODirection(BUTTONWRITE_GPIO, OUT)) {
        transmitter_logger("Button GPIO Direction Setting Failed");
        printf("Button GPIO Direction Setting Failed\n");
        exit(0);
    }
    if (-1 == GPIOWrite(BUTTONWRITE_GPIO, 1)) {
        transmitter_logger("Button GPIO Write Failed");
        printf("Button GPIO Write Failed\n");
        exit(0);
    }

    //Initialize PWM
    if (-1 == PWMExport(LEDWRITE_PWM)) {
        transmitter_logger("LED PWM Export Failed");
        printf("LED PWM Export Failed\n");
        exit(0);
    }
    if (-1 == PWMWritePeriod(LEDWRITE_PWM, 20000000)) {
        transmitter_logger("LED PWM Period Write Failed");
        printf("LED PWM Period Write Failed\n");
        exit(0);
    }
    if (-1 == PWMEnable(LEDWRITE_PWM)) {
        transmitter_logger("LED PWM Enable Failed");
        printf("LED PWM Enable Failed\n");
        exit(0);
    }

    //Initialize Threads & Work Start
    pthread_t p_thread[4];
    int status;
    if(pthread_create(&p_thread[0], NULL, transmitter_work, NULL) < 0){
        fprintf(stderr, "Work Thread Failed\n");
        exit(0);
    }
    if(pthread_create(&p_thread[1], NULL, transmitter_cli, NULL) < 0){
        fprintf(stderr, "CLI Thread Failed\n");
        exit(0);
    }
    if(pthread_create(&p_thread[2], NULL, transmitter_buttonchecker, NULL) < 0){
        fprintf(stderr, "Buttonchecker Thread Failed\n");
        exit(0);
    }
    if(pthread_create(&p_thread[3], NULL, transmitter_ledcontroller, NULL) < 0){
        fprintf(stderr, "LEDcontroller Thread Failed\n");
        exit(0);
    }

    //Work End
    pthread_join(p_thread[0], (void **)&status);
    pthread_join(p_thread[1], (void **)&status);
    pthread_join(p_thread[2], (void **)&status);
    pthread_join(p_thread[3], (void **)&status);
    transmitter_logger("System Shutdown");
    return 0;
} // main
