# EP1 - Implementação do MQTT broker
**Estudante:** Ana Carla Quallio Rosa

Este repositório trata-se da implementação do broker MQTT (EP1 da disciplina de Programação para Redes de computadores).

## Requisitos e execução
### Requisitos
Para executar o projeto, você deve ter as seguintes ferramentas instaladas:
- `GCC` (GNU Compiler Collection)
- `make`
- `mosquitto-clients` (para testes)

A instalação pode ser feita com os seguintes comandos:
```bash
sudo apt update
sudo apt install gcc make mosquitto-clients
```

### Executando o broker
Para compilar o programa, você deve executar o comando:


```bash
make
```
Em seguida, você pode compilar o programa, passando a porta que deseja executar o broker (por padrão, o MQTT utiliza  aporta 1883, mas pode ser que esta porta esteja ocupada no seu computador, desse modo, foi realizada a implementação para que seja possível utilizar outra porta se for necessário)

```bash
./ep1-mqtt-broker 1883
```

Para executar o broker com os programas `mosquitto_sub` e `mosquitto_pub`, não esqueça de passar a versão 5.0 do MQTT. Por exemplo:

```bash
mosquitto_sub -t "test/topic" -p 1883 -V 5 -d
```

```bash
mosquitto_pub -t "test/topic" -m "hello" -p 1883 -V 5
```

Se você quiser executar o broker em outra porta em conjunto com o `mosquitto`, não esqueça de passar o número da porta, por exemplo:

```bash
mosquitto_pub -t "test/topic" -m "hello" -p 1884 -V 5
```
