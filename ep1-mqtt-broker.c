#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>

#define LISTENQ 10
#define MAXLINE 4096
#define MAX_CLIENTS 100

// --- Estruturas de Dados ---

// Informações sobre um cliente para serem armazenadas pelo PROCESSO PAI
typedef struct
{
    int connfd;         // Socket de conexão real com o cliente
    int parent_comm_fd; // Socket de comunicação com o processo filho
    char client_id[128];
    char subscribed_topics[10][128]; // Suporta múltiplas subscrições por cliente
    int topic_count;
} client_info_t;

// Struct para comunicação entre pai e filho
typedef struct
{
    enum
    {
        MSG_TYPE_CLIENT_ID,
        MSG_TYPE_SUBSCRIBE,
        MSG_TYPE_PUBLISH,
        MSG_TYPE_DISCONNECT
    } type;
    union
    {
        char client_id[128];
        struct
        {
            char topic[128];
            char message[1024];
        } pub_data;
    } data;
} parent_child_msg_t;

// Variáveis globais do PROCESSO PAI
client_info_t clients[MAX_CLIENTS];
struct pollfd poll_fds[MAX_CLIENTS + 1];
int active_clients = 0;

// Variáveis globais do PROCESSO FILHO
int parent_comm_fd = -1;
int client_conn_fd = -1;

// --- Funções Auxiliares MQTT ---

uint32_t decode_remaining_length(unsigned char *buffer, int *bytes_used)
{
    uint32_t multiplier = 1;
    uint32_t value = 0;
    unsigned char encodedByte;
    int i = 0;
    do
    {
        if (i >= 4)
        {
            *bytes_used = 0;
            return 0;
        }
        encodedByte = buffer[i++];
        value += (encodedByte & 127) * multiplier;
        multiplier *= 128;
    } while ((encodedByte & 128) != 0);
    *bytes_used = i;
    return value;
}

int encode_remaining_length(uint32_t length, unsigned char *buffer)
{
    int bytes = 0;
    do
    {
        buffer[bytes] = length % 128;
        length /= 128;
        if (length > 0)
            buffer[bytes] |= 128;
        bytes++;
    } while (length > 0);
    return bytes;
}

void send_connack(int connfd)
{
    unsigned char connack[] = {0x20, 0x03, 0x00, 0x00, 0x00};
    write(connfd, connack, sizeof(connack));
    printf("[FILHO %d] CONNACK enviado para cliente fd=%d\n", getpid(), connfd);
}

void send_suback(int connfd, uint16_t packet_id)
{
    unsigned char suback[] = {0x90, 0x04, (packet_id >> 8) & 0xFF, packet_id & 0xFF, 0x00, 0x00};
    write(connfd, suback, sizeof(suback));
    printf("[FILHO %d] SUBACK enviado para cliente fd=%d\n", getpid(), connfd);
}

void send_publish(int connfd, const char *topic, const char *message)
{
    uint16_t topic_len = strlen(topic);
    uint32_t msg_len = strlen(message);
    uint32_t remaining_length = 2 + topic_len + 1 + msg_len;
    unsigned char header[5];
    header[0] = 0x30;
    int rl_bytes = encode_remaining_length(remaining_length, header + 1);
    write(connfd, header, rl_bytes + 1);
    unsigned char topic_len_bytes[2];
    topic_len_bytes[0] = (topic_len >> 8) & 0xFF;
    topic_len_bytes[1] = topic_len & 0xFF;
    write(connfd, topic_len_bytes, 2);
    write(connfd, topic, topic_len);
    unsigned char prop_len = 0;
    write(connfd, &prop_len, 1);
    write(connfd, message, msg_len);
    printf("[FILHO %d] PUBLISH encaminhado para cliente fd=%d -> tópico '%s'\n", getpid(), connfd, topic);
}

void send_pingresp(int connfd)
{
    unsigned char pingresp[] = {0xD0, 0x00};
    write(connfd, pingresp, sizeof(pingresp));
    printf("[FILHO %d] PINGRESP enviado para cliente fd=%d\n", getpid(), connfd);
}

// Handler para evitar processos zumbis
void sigchld_handler(int sig)
{
    (void)sig; // Para não dar warning na compilação
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

// --- Funções de Comunicação no processo filho ---

void send_client_id_to_parent(const char *client_id)
{
    parent_child_msg_t msg;
    msg.type = MSG_TYPE_CLIENT_ID;
    strncpy(msg.data.client_id, client_id, sizeof(msg.data.client_id) - 1);
    msg.data.client_id[sizeof(msg.data.client_id) - 1] = '\0';
    write(parent_comm_fd, &msg, sizeof(msg));
}

void send_subscribe_to_parent(const char *topic)
{
    parent_child_msg_t msg;
    msg.type = MSG_TYPE_SUBSCRIBE;
    strncpy(msg.data.pub_data.topic, topic, sizeof(msg.data.pub_data.topic) - 1);
    msg.data.pub_data.topic[sizeof(msg.data.pub_data.topic) - 1] = '\0';
    write(parent_comm_fd, &msg, sizeof(msg));
}

void send_publish_to_parent(const char *topic, const char *message)
{
    parent_child_msg_t msg;
    msg.type = MSG_TYPE_PUBLISH;
    strncpy(msg.data.pub_data.topic, topic, sizeof(msg.data.pub_data.topic) - 1);
    msg.data.pub_data.topic[sizeof(msg.data.pub_data.topic) - 1] = '\0';
    strncpy(msg.data.pub_data.message, message, sizeof(msg.data.pub_data.message) - 1);
    msg.data.pub_data.message[sizeof(msg.data.pub_data.message) - 1] = '\0';
    write(parent_comm_fd, &msg, sizeof(msg));
}

void send_disconnect_to_parent(void)
{
    parent_child_msg_t msg;
    msg.type = MSG_TYPE_DISCONNECT;
    write(parent_comm_fd, &msg, sizeof(msg));
}

// --- Handlers de Pacotes MQTT no processo filho ---

int handle_connect_child(int connfd, unsigned char *packet, uint32_t length)
{
    (void)length;
    
    int offset = 0;
    uint16_t proto_len = (packet[offset] << 8) | packet[offset + 1];
    offset += 2;
    char proto_name[proto_len + 1];
    memcpy(proto_name, packet + offset, proto_len);
    proto_name[proto_len] = '\0';
    offset += proto_len;
    unsigned char version = packet[offset++];
    printf("[FILHO %d] CONNECT: Protocolo %s, Versão %d\n", getpid(), proto_name, version);
    if (version != 5)
        return -1;
    
    // Pular flags (1 byte) + keep alive (2 bytes)
    offset += 3;
    
    int prop_len_bytes = 0;
    uint32_t prop_len = decode_remaining_length(packet + offset, &prop_len_bytes);
    offset += prop_len_bytes + prop_len;
    uint16_t client_id_len = (packet[offset] << 8) | packet[offset + 1];
    offset += 2;
    char client_id[128] = {0};
    if (client_id_len > 0)
    {
        memcpy(client_id, packet + offset, client_id_len);
        client_id[client_id_len] = '\0';
    }
    else
    {
        static int client_counter = 1;
        snprintf(client_id, sizeof(client_id), "client_%d", client_counter++);
    }
    
    // Extrair keep alive para mostrar no log
    uint16_t keep_alive = (packet[offset - 5] << 8) | packet[offset - 4];
    printf("[FILHO %d] Client ID: %s, KeepAlive: %d\n", getpid(), client_id, keep_alive);

    send_client_id_to_parent(client_id);
    send_connack(connfd);
    return 1;
}

int handle_subscribe_child(int connfd, unsigned char *packet, uint32_t length)
{
    (void)length;
    
    int offset = 0;
    uint16_t packet_id = (packet[offset] << 8) | packet[offset + 1];
    offset += 2;
    int prop_len_bytes = 0;
    uint32_t properties_length = decode_remaining_length(packet + offset, &prop_len_bytes);
    offset += prop_len_bytes + properties_length;
    uint16_t topic_len = (packet[offset] << 8) | packet[offset + 1];
    offset += 2;
    
    if (topic_len >= 128) {
        printf("[FILHO %d] Tópico muito longo: %d\n", getpid(), topic_len);
        return -1;
    }
    
    char topic_name[128];
    memcpy(topic_name, packet + offset, topic_len);
    topic_name[topic_len] = '\0';
    offset += topic_len;
    
    // Pular options (1 byte)
    offset++;
    
    printf("[FILHO %d] SUBSCRIBE: Tópico '%s', ID de pacote %d\n", getpid(), topic_name, packet_id);

    send_subscribe_to_parent(topic_name);
    send_suback(connfd, packet_id);
    return 1;
}

int handle_publish_child(int connfd, unsigned char *packet, uint32_t length, unsigned char flags)
{
    (void)connfd;
    (void)flags;
    
    int offset = 0;
    uint16_t topic_len = (packet[offset] << 8) | packet[offset + 1];
    offset += 2;
    
    if (topic_len >= 128) {
        printf("[FILHO %d] Tópico muito longo: %d\n", getpid(), topic_len);
        return -1;
    }
    
    char topic_name[128];
    memcpy(topic_name, packet + offset, topic_len);
    topic_name[topic_len] = '\0';
    offset += topic_len;
    int prop_len_bytes = 0;
    uint32_t properties_length = decode_remaining_length(packet + offset, &prop_len_bytes);
    offset += prop_len_bytes + properties_length;
    uint32_t payload_length = length - offset;
    
    if (payload_length >= 1024) {
        printf("[FILHO %d] Payload muito grande: %u\n", getpid(), payload_length);
        return -1;
    }
    
    char message[1024];
    memcpy(message, packet + offset, payload_length);
    message[payload_length] = '\0';
    printf("[FILHO %d] PUBLISH recebido: %s -> %s\n", getpid(), topic_name, message);

    send_publish_to_parent(topic_name, message);
    return 1;
}

int handle_disconnect_child(int connfd)
{
    (void)connfd;
    printf("[FILHO %d] DISCONNECT recebido\n", getpid());
    send_disconnect_to_parent();
    return 0;
}

int process_packet(int connfd, unsigned char *buffer, int n_read)
{
    if (n_read < 2)
        return 1;
    unsigned char packet_type = (buffer[0] >> 4) & 0x0F;
    unsigned char flags = buffer[0] & 0x0F;
    int bytes_used;
    uint32_t remaining_length = decode_remaining_length(buffer + 1, &bytes_used);
    int total_packet_size = 1 + bytes_used + remaining_length;
    if (n_read < total_packet_size)
        return 1;
    unsigned char *payload = buffer + 1 + bytes_used;

    switch (packet_type)
    {
    case 1:
        return handle_connect_child(connfd, payload, remaining_length);
    case 3:
        return handle_publish_child(connfd, payload, remaining_length, flags);
    case 8:
        return handle_subscribe_child(connfd, payload, remaining_length);
    case 12:
        send_pingresp(connfd);
        return 1;
    case 14:
        return handle_disconnect_child(connfd);
    default:
        printf("[FILHO %d] Tipo de pacote não implementado: %d\n", getpid(), packet_type);
        return 1;
    }
}

void process_client_child(int connfd, int comm_fd)
{
    client_conn_fd = connfd;
    parent_comm_fd = comm_fd;

    struct pollfd fds[2];
    fds[0].fd = client_conn_fd;
    fds[0].events = POLLIN;
    fds[1].fd = parent_comm_fd;
    fds[1].events = POLLIN;

    unsigned char buffer[MAXLINE];
    int bytes_remaining = 0;

    for (;;)
    {
        int poll_count = poll(fds, 2, -1);
        if (poll_count < 0)
        {
            if (errno == EINTR)
                continue;
            perror("poll em processo filho");
            break;
        }

        if (fds[0].revents & POLLIN)
        { // Dados do cliente
            ssize_t n = read(client_conn_fd, buffer + bytes_remaining, MAXLINE - bytes_remaining);
            if (n <= 0)
                break;
            int total_bytes = bytes_remaining + n;
            int offset = 0;
            while (offset < total_bytes)
            {
                int bytes_used;
                uint32_t remaining_length = decode_remaining_length(buffer + offset + 1, &bytes_used);
                int packet_size = 1 + bytes_used + remaining_length;
                if (offset + packet_size > total_bytes)
                    break;
                if (process_packet(client_conn_fd, buffer + offset, packet_size) <= 0)
                    break;
                offset += packet_size;
            }
            if (offset < total_bytes)
            {
                memmove(buffer, buffer + offset, total_bytes - offset);
                bytes_remaining = total_bytes - offset;
            }
            else
            {
                bytes_remaining = 0;
            }
        }

        if (fds[1].revents & POLLIN)
        { // Mensagem do pai
            parent_child_msg_t msg;
            ssize_t n = read(parent_comm_fd, &msg, sizeof(msg));
            if (n > 0)
            {
                if (msg.type == MSG_TYPE_PUBLISH)
                {
                    send_publish(client_conn_fd, msg.data.pub_data.topic, msg.data.pub_data.message);
                }
            }
            else if (n == 0)
            {
                // Conexão fechada pelo pai
                break;
            }
        }
    }
    printf("[FILHO %d] Conexão fechada. Finalizando.\n", getpid());
    close(client_conn_fd);
    close(parent_comm_fd);
}

// --- Lógica do processo pai ---

bool is_subscribed(const client_info_t *client, const char *topic)
{
    for (int i = 0; i < client->topic_count; i++)
    {
        if (strcmp(client->subscribed_topics[i], topic) == 0)
        {
            return true;
        }
    }
    return false;
}

void remove_client_from_parent(int comm_fd)
{
    for (int i = 0; i < active_clients; i++)
    {
        if (clients[i].parent_comm_fd == comm_fd)
        {
            printf("[PAI] Removendo cliente %s (fd=%d)\n", clients[i].client_id, clients[i].connfd);
            close(clients[i].parent_comm_fd);
            
            // Corrigir o memmove para evitar buffer overflow
            if (i < active_clients - 1)
            {
                memmove(&clients[i], &clients[i + 1], (active_clients - i - 1) * sizeof(client_info_t));
                memmove(&poll_fds[i + 1], &poll_fds[i + 2], (active_clients - i - 1) * sizeof(struct pollfd));
            }
            active_clients--;
            break;
        }
    }
}

int main(int argc, char **argv)
{
    int listenfd;
    struct sockaddr_in servaddr;
    
    if (argc != 2)
    {
        fprintf(stderr, "Uso: %s <Porta>\n", argv[0]);
        fprintf(stderr, "Vai rodar um servidor de echo na porta <Porta> TCP\n");
        exit(1);
    }

    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("socket :(\n");
        exit(2);
    }

    // Evitar problemas com porta em uso
    int optval = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(atoi(argv[1]));
    if (bind(listenfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) == -1)
    {
        perror("bind :(\n");
        close(listenfd);
        exit(3);
    }

    if (listen(listenfd, LISTENQ) == -1)
    {
        perror("listen :(\n");
        close(listenfd);
        exit(4);
    }

    signal(SIGCHLD, sigchld_handler);
    printf("[BROKER PAI] Aguardando conexões na porta %s...\n", argv[1]);

    poll_fds[0].fd = listenfd;
    poll_fds[0].events = POLLIN;

    for (;;)
    {
        int poll_count = poll(poll_fds, active_clients + 1, -1);
        if (poll_count < 0)
        {
            if (errno == EINTR)
                continue;
            perror("poll no processo pai");
            break;
        }

        if (poll_fds[0].revents & POLLIN)
        {
            int connfd = accept(listenfd, NULL, NULL);
            if (connfd < 0)
            {
                perror("accept");
                continue;
            }
            if (active_clients >= MAX_CLIENTS)
            {
                printf("[PAI] Número máximo de clientes atingido. Rejeitando conexão.\n");
                close(connfd);
                continue;
            }
            int comm_socks[2];
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, comm_socks) < 0)
            {
                perror("socketpair");
                close(connfd);
                continue;
            }

            pid_t pid = fork();
            if (pid == 0)
            { // Processo Filho
                printf("[Uma conexão aberta]\n");
                close(listenfd);
                close(comm_socks[0]);
                process_client_child(connfd, comm_socks[1]);
                printf("[Uma conexão fechada]\n");
                exit(0);
            }
            else if (pid > 0)
            { // Processo Pai
                close(connfd);
                close(comm_socks[1]);
                clients[active_clients].connfd = connfd;
                clients[active_clients].parent_comm_fd = comm_socks[0];
                clients[active_clients].topic_count = 0;
                clients[active_clients].client_id[0] = '\0'; // Inicializar client_id
                poll_fds[active_clients + 1].fd = comm_socks[0];
                poll_fds[active_clients + 1].events = POLLIN;
                active_clients++;
                printf("[PAI] Novo cliente conectado. PID: %d, clientes ativos: %d\n", pid, active_clients);
            }
            else
            {
                perror("fork");
                close(connfd);
            }
        }

        for (int i = 0; i < active_clients; i++)
        {
            if (poll_fds[i + 1].revents & POLLIN)
            {
                parent_child_msg_t msg;
                ssize_t n = read(poll_fds[i + 1].fd, &msg, sizeof(msg));
                if (n > 0)
                {
                    switch (msg.type)
                    {
                    case MSG_TYPE_CLIENT_ID:
                        strncpy(clients[i].client_id, msg.data.client_id, sizeof(clients[i].client_id) - 1);
                        clients[i].client_id[sizeof(clients[i].client_id) - 1] = '\0';
                        printf("[PAI] Cliente %s (%d) conectado.\n", clients[i].client_id, clients[i].connfd);
                        break;
                    case MSG_TYPE_SUBSCRIBE:
                        if (clients[i].topic_count < 10)
                        {
                            strncpy(clients[i].subscribed_topics[clients[i].topic_count], 
                                   msg.data.pub_data.topic, sizeof(clients[i].subscribed_topics[0]) - 1);
                            clients[i].subscribed_topics[clients[i].topic_count][sizeof(clients[i].subscribed_topics[0]) - 1] = '\0';
                            clients[i].topic_count++;
                            printf("[PAI] Cliente %s (%d) subscrito no tópico '%s'.\n", 
                                  clients[i].client_id, clients[i].connfd, msg.data.pub_data.topic);
                        }
                        else
                        {
                            printf("[PAI] ERRO: Cliente %s excedeu limite de subscrições\n", clients[i].client_id);
                        }
                        break;
                    case MSG_TYPE_PUBLISH:
                        printf("[PAI] PUBLISH de %s: '%s' -> '%s'\n", clients[i].client_id, msg.data.pub_data.topic, msg.data.pub_data.message);
                        for (int j = 0; j < active_clients; j++)
                        {
                            if (i == j)
                                continue;
                            if (is_subscribed(&clients[j], msg.data.pub_data.topic))
                            {
                                ssize_t written = write(clients[j].parent_comm_fd, &msg, sizeof(msg));
                                if (written != sizeof(msg))
                                {
                                    printf("[PAI] ERRO ao enviar mensagem para cliente %s\n", clients[j].client_id);
                                }
                            }
                        }
                        break;
                    case MSG_TYPE_DISCONNECT:
                        remove_client_from_parent(poll_fds[i + 1].fd);
                        i--;
                        break;
                    }
                }
                else
                {
                    remove_client_from_parent(poll_fds[i + 1].fd);
                    i--;
                }
            }
        }
    }
    close(listenfd);
    return 0;
}