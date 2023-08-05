#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define MAX_LISTEN 5
#define MAX_LIST 300

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
//Socket
//**********************************************************************
int serv_sock, clnt_sock = -1;
struct sockaddr_in serv_addr, clnt_addr;
socklen_t clnt_addr_size;
int server_port;

//**********************************************************************
//Payload
//**********************************************************************
char socket_payload[SOCKET_PAYLOAD_SIZE];

//**********************************************************************
//Time
//**********************************************************************
time_t ct;
struct tm tm;

//**********************************************************************
//DB_Logger / DB_Logprinter / DB_Logremover
//**********************************************************************
#define LOG_BUFFER_MAX 256
const char * logfilepath = "db.txt";

int db_logger(const char * message) {
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

int db_logprinter() {
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

int db_logremover() {
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
//DB_List
//**********************************************************************

typedef struct{
    int id = 0;
    char name[SOCKET_PAYLOAD_SIZE];
    int accepted;
    db_list * next = NULL;
} db_list;

db_list headlist;

const char * listfilepath = "list.txt"; //Format : ID Name Accepted

int db_initlist(){
    FILE *fp = fopen(listfilepath, "r");

    if(fp == NULL) {
        db_logger("List File Not Found!");
        return -1;
    }
    db_logger("Importing List");

    headlist.id = -1;

    char buffer[SOCKET_PAYLOAD_SIZE];

    while(fgets(buffer, sizeof(buffer), fp) != NULL) { // {ID, Name, Accepted}
        db_list * newentry = (db_list*)malloc(sizeof(db_list));
        char * temp = strtok(buffer, " ");
        newentry->id = atoi(temp);
        temp = strtok(NULL, " ");
        strcpy(newentry->name , temp);
        temp = strtok(NULL, " ");
        newentry->accepted = atoi(temp);
        newentry->next = NULL;

        db_list * lastentry = &headlist;
        while(lastentry->next != NULL) lastentry = lastentry->next;
        lastentry->next = newentry;
    }

    fclose(fp);

    return 0;
}

//Return char[3][SOCKET_PAYLOAD_SIZE] = {ID, Name, Accepted} // NULL if not exist in list
//ex) 12345 asdf 1 => user "asdf" with id 12345 has been banned
char** db_checklist(int user_id){
    char ** toreturn = (char**) malloc(sizeof(char *) * 3);
    for(int i = 0 ; i < 3; i++) toreturn[i] = (char*) malloc(sizeof(char) * SOCKET_PAYLOAD_SIZE);

    db_list * iter = headlist.next;
    while(iter != NULL){
        if(iter->id == user_id) break;
        iter = iter->next;
    }
    if (iter == NULL) return NULL;
    sprintf(toreturn[0], "%d", iter->id);
    strcpy(toreturn[1], iter->name);
    sprintf(toreturn[2], "%d", iter->accepted);
    return toreturn;
}

void db_printlist(){
    db_list * iter = headlist.next;
    while(iter != NULL){
        fprintf(stderr, "%d ", iter->id);
        fprintf(stderr, "%s ", iter->name);
        fprintf(stderr, "%s\n", iter->accepted == 0 ? "Accepted" : "Rejected");
        iter = iter->next;
    }
}

int db_addlist(int user_id, char * name, int accepted){ //return -1 if id already exists
    db_list * iter = &headlist;
    while(iter->next != NULL){
        if(iter->next->id == user_id) break;
        iter = iter->next;
    }
    if (iter->next != NULL) return -1;

    db_list * newentry = (db_list*)malloc(sizeof(db_list));
    newentry->id = user_id;
    strcpy(newentry->name, name);
    newentry->accepted = accepted;
    newentry->next = NULL;
    iter->next = newentry;
    return 0;
}

int db_dellist(int user_id){ //if user_id == -1, delete all / return -1 if id doesn't exist
    if(user_id == -1){
        db_list * iter = headlist.next;
        db_list * todelete;
        while(iter != NULL){
            todelete = iter;
            iter = iter->next;
            free(todelete);
        }
        headlist.next = NULL;
    }
    else{
        db_list * iter = &headlist;
        while(iter->next != NULL){
            if(iter->next->id == user_id) break;
            iter = iter->next;
        }
        if (iter->next == NULL) return -1;
        db_list * todelete = iter->next;
        iter->next = iter->next->next;
        free(todelete);
    }
    return 0;
}

//**********************************************************************
//DB_Work
//**********************************************************************
void *db_work(void *) {
    if(clnt_sock < 0){
        clnt_addr_size = sizeof(clnt_addr);
        clnt_sock = accept(serv_sock, (struct sockaddr*) &clnt_addr, &clnt_addr_size);
        if(clnt_sock == -1){
            db_logger("Socket Accept Failed");
            fprintf(stderr, "Socket Accept Failed\n");
            exit(0);
        }
    }
    db_logger("Connected To a Receiver");

    while(1) {
        int socket_sended, socket_received;
        //Socket Receive
        memset(socket_payload, 0, SOCKET_PAYLOAD_SIZE);
        socket_received = recv(clnt_sock, socket_payload, SOCKET_PAYLOAD_SIZE, 0);
        int result;
        char * name;
        if(socket_received){
            db_logger("Socket Receive Success");
            int resend = 0;
            char * parsed = strtok(socket_payload, " ");
            if(parsed != NULL && strcmp(parsed, "RECEIVER")==0){
                parsed = strtok(NULL, " ");
                if(parsed != NULL && strcmp(parsed, "DATA")==0){
                    parsed = strtok(NULL, " ");
                    if(parsed != NULL && atoi(parsed) >= 0){
                        char str[LOG_BUFFER_MAX];
                        sprintf(str, "User %d ", atoi(parsed));
                        char ** userinfo = db_checklist(atoi(parsed));

                        if(userinfo == NULL){
                            result = 2;
                            name = "null";
                            strcat(str, "Not in List");
                        }
                        else if(strcmp(userinfo[2], "1") == 0){
                            result = 1;
                            name = userinfo[1];
                            strcat(str, "Rejected");
                        }
                        else{
                            result = 0;
                            name = userinfo[1];
                            strcat(str, "Accepted");
                        }
                        db_logger(str);

                    } else {
                        db_logger("Wrong Data");
                        resend = 1;
                    }
                }
                else {
                    db_logger("Not DATA");
                    resend = 1;
                }
            }
            else db_logger("Data Not From RECEIVER");
            //Socket Send
            int sendloop = 1;
            while(sendloop){
                memset(socket_payload, 0, SOCKET_PAYLOAD_SIZE);
                if (resend == 0) {
                    strcat(socket_payload, "DB DATA ");
                    char temp[20];
                    sprintf(temp, "%s %d", name, result);
                    strcat(socket_payload, temp);
                }
                else strcat(socket_payload, "DB RESEND");
                socket_sended = send(clnt_sock, socket_payload, SOCKET_PAYLOAD_SIZE, 0);
                if(socket_sended) {
                    db_logger("Socket Send Success");
                    sendloop = 0;
                }
                else db_logger("Socket Send Failed");
            }
        }
        //else db_logger("Socket Receive Failed");
        usleep(1000);
    }
}

//**********************************************************************
//DB_CLI
//**********************************************************************
#define CLI_BUFFER_MAX 256

void *db_cli(void *){
    char input[CLI_BUFFER_MAX];
    while(1){
        fprintf(stderr, "DB $ ");
        scanf("%[^\n]", input);
        if(input[0] == '\0') continue;
        char * parsed = strtok(input, " ");
        if(strcmp(parsed, "log") == 0) {
            parsed = strtok(NULL, " ");
            if(parsed == NULL || strcmp(parsed, "-p") == 0) db_logprinter();
            else if(strcmp(parsed, "-r") == 0) {
                fprintf(stderr, "Removing Log\n");
                db_logremover();
            }
            else fprintf(stderr, "Unknown Parameter %s\n", parsed);
        }
        else if (strcmp(parsed, "list") == 0){
            parsed = strtok(NULL, " ");
            if(parsed == NULL || strcmp(parsed, "-p") == 0) db_printlist();
            else if(strcmp(parsed, "-r") == 0) {
                parsed = strtok(NULL, " ");
                if(parsed == NULL) fprintf(stderr, "Usage : list -r <ID / all>\n");
                else if (strcmp(parsed, "all") == 0){
                    fprintf(stderr, "List Reset\n");
                    db_logger("List Reset");
                    db_dellist(-1);
                }
                else {
                    if (atoi(parsed) > 0){
                        if(db_dellist(atoi(parsed)) == -1) fprintf(stderr, "%d Is Not In List\n", atoi(parsed));
                        else{
                            char str[LOG_BUFFER_MAX];
                            sprintf(str, "%d Removed From List", atoi(parsed));
                            db_logger(str);
                            fprintf(stderr, "%s\n", str);
                        }
                    }
                    else fprintf(stderr, "Invalid Value %s\n", parsed);
                }
            }
            else if(strcmp(parsed, "-a") == 0) {
                int id, valid;
                char name[SOCKET_PAYLOAD_SIZE];
                parsed = strtok(NULL, " ");
                if(parsed == NULL) fprintf(stderr, "Usage : list -a <ID> <NAME> <VALID>\n");
                else {
                    if (atoi(parsed) > 0){
                        id = atoi(parsed);
                        parsed = strtok(NULL, " ");
                        if(parsed == NULL) fprintf(stderr, "Usage : list -a <ID> <NAME> <VALID>\n");
                        else {
                            strcpy(name, parsed);
                            parsed = strtok(NULL, " ");
                            if(parsed == NULL) fprintf(stderr, "Usage : list -a <ID> <NAME> <VALID>\n");
                            else {
                                if(atoi(parsed) == 0 || atoi(parsed) == 1){
                                    valid = atoi(parsed);
                                    if(db_addlist(id, name, valid) == -1) fprintf(stderr, "ID %d already exists\n", id);
                                    else {
                                        char str[LOG_BUFFER_MAX];
                                        sprintf(str, "[%d / %s / %d] Added To List", id, name, valid);
                                        db_logger(str);
                                        fprintf(stderr, "%s\n", str);
                                    }
                                }
                                else fprintf(stderr, "Invalid Valid Value %s\n", parsed);
                            }
                        }
                    }
                    else fprintf(stderr, "Invalid ID Value %s\n", parsed);
                }
            }
            else fprintf(stderr, "Unknown Parameter %s\n", parsed);
        }
        else if (strcmp(parsed, "clear") == 0) system("clear");
        else if (strcmp(parsed, "exit") == 0){
            db_logger("CLI Shutdown");
            exit_marker = 1;
            kill(getpid(), SIGINT);
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
    db_logger("System Start");

    //Initialize Socket
    serv_sock = socket(PF_INET, SOCK_STREAM, 0);
    if(serv_sock == -1){
        db_logger("Socket Initialization Failed");
        fprintf(stderr, "Socket Initialization Failed\n");
        exit(0);
    }
    if(argc != 2){
        fprintf(stderr, "Enter Server's Port : ");
        scanf("%d", &server_port);
        while(getchar() != '\n');
    }
    else server_port = atoi(argv[1]);
    memset(&serv_addr, 0 , sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(server_port);

    char str[LOG_BUFFER_MAX];
    sprintf(str, "Socket Set With Port %d", server_port);
    db_logger(str);

    if(bind(serv_sock, (struct sockaddr*) &serv_addr, sizeof(serv_addr))==-1){
        db_logger("Socket Bind Failed");
        fprintf(stderr, "Socket Bind Failed\n");
        exit(0);
    }

    if(listen(serv_sock, MAX_LISTEN)==-1){
        db_logger("Socket Listen Failed");
        fprintf(stderr, "Socket Listen Failed\n");
        exit(0);
    }

    //Initialize DB List
    db_initlist();

    //Initialize Threads & Work Start
    pthread_t p_thread[2];
    int status;
    if(pthread_create(&p_thread[0], NULL, db_work, NULL) < 0){
        fprintf(stderr, "Work Thread Failed\n");
        exit(0);
    }
    if(pthread_create(&p_thread[1], NULL, db_cli, NULL) < 0){
        fprintf(stderr, "CLI Thread Failed\n");
        exit(0);
    }

    //Work End
    pthread_join(p_thread[0], (void **)&status);
    pthread_join(p_thread[1], (void **)&status);

    close(serv_sock);
    close(clnt_sock);

    db_logger("System Shutdown");
    return 0;
} // main
