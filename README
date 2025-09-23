# EP1 - Implementação do MQTT broker
**Estudante:** Ana Carla Quallio Rosa

Este repositório trata-se da implementação do broker MQTT, EP1 da disciplina de Programação para Redes de computadores. 

## Sumário 
Esta documentação está dividida nas seções:

1. [Organização do repositório](#organização-do-repositório)

2. [Requisitos para a execução](#requisitos-para-a-execução)

3. [Executando o broker](#executando-o-broker)



## Conteúdo
### Organização do repositório
O repositório `ep1-AnaRosa` está organizado da seguinte forma:

- `ep1-mqtt-broker.c`: arquivo da implementação final do projeto;
- `mqtt-broker-v0.c`: arquivo da implementação da primeira versão do projeto (para ser explicado na seção de Arqtuiteturas e escolhas de projeto);
- `makefile`: arquivo de makefile (compilação);
- `README`: documentação do projeto.
- `ep1.pdf`: slides do trabalho, conforme requisitos da descrição do EP1.

### Requisitos para a execução
Para a execução do projeto, você deve ter o `GCC`, o `make` e o `mosquitto` instalados em seu computador, por meio dos comandos:

```bash
sudo apt install gcc
```

```bash
sudo apt install make
```

```bash
sudo apt install mosquitto
```

### Executando o broker
Para compilar o programa, você deve executar o comando:


```bash
make
```
Em seguida, você pode compilar o programa, passando a porta que deseja executar o broker (por padrão, o MQTT utiliza  aporta 1883, mas pode acontecer conflitos com o mosquitto, desse modo, foi realizada a implementação para que seja possível utilizar outra porta se for necessário)

```bash
./ep1-mqtt-broker 1883
```

Para executar o broker com os programas `mosquitto_sub` e `mosquitto_pub`, não esqueça de passar que trata-se da versão 5.0 do MQTT. Por exemplo:

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

### Arquitetura e escolhas de projeto