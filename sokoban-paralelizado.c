#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <assert.h>
#include <stdbool.h>

#include <omp.h>
#include <sys/time.h>
#include <math.h>

int w, h, n_boxes;             // largura (w), altura (h) e número de caixas (n_boxes)
uint8_t *board, *goals, *live; // Ponteiros para o tabuleiro, metas e células "vivas"

typedef uint16_t cidx_t; // Tipo para index de célula
typedef uint32_t hash_t; // Tipo para hash (função de dispersão)

/* A configuração do tabuleiro é representada por um array de índices de células
   do jogador e das caixas */
typedef struct state_t state_t;

struct state_t
{                                 // estrutura para identificar o estado do jogo
    hash_t h;                     // hash para o estado (usado para otimização)
    state_t *prev, *next, *qnext; // ponteiros para o estado anterior, próximo e próximo na fila,  cria uma lista ligada para estados explorados ou a explorar
    cidx_t c[];                   // array de índices de células (posição do jogador e das caixas)
};

// Definições de tipos de células no tabuleiro
enum
{
    space,  // espaço vazio
    wall,   // parede
    player, // jogador
    box     // caixa
};

size_t state_size, block_size = 32; // Tamanho do estado e tamanho do bloco para alocação
state_t *block_root, *block_head;   // Ponteiros para a raiz e cabeça do bloco de memória para os estados

/*--------------------- Funções Principais ---------------------*/

/*----------- Gerenciamento de Estados -----------*/

// Função que retorna o próximo estado alocado
state_t *next_of(state_t *s)
{
    return (void *)((uint8_t *)s + state_size); // Retorna a posição do próximo estado
}

// Função para criar um novo estado, baseado em um estado pai
state_t *newstate(state_t *parent)
{
    state_t *ptr;
    if (!block_head)
    {
        block_size *= 2;                              // Dobra o tamanho do bloco de memória
        state_t *p = malloc(block_size * state_size); // Aloca o bloco de memória
        assert(p);
        p->next = block_root;
        block_root = p;
        ptr = (void *)((uint8_t *)p + state_size * block_size);
        p = block_head = next_of(p);
        state_t *q;
        for (q = next_of(p); q < ptr; p = q, q = next_of(q))
            p->next = q;
        p->next = NULL;
    }

    ptr = block_head;
    block_head = block_head->next;

    ptr->prev = parent; // Define o estado anterior
    ptr->h = 0;         // Inicializa o hash
    return ptr;
}

// Função para liberar um estado e devolver para a lista de estados disponíveis
void unnewstate(state_t *p)
{
    p->next = block_head;
    block_head = p;
}

/*----------- Manipulação de Tabuleiro -----------*/

// Função para marcar posições onde uma caixa não deve estar
// Marca as células no tabuleiro onde as caixas podem se mover (espaços "vivos"), considerando paredes e posições inválidas.

// Função para marcar células "vivas"
void mark_live_iterative(const int start)
{
    // Estratégia de pilha paralelizada mais eficiente para marcar células vivas
    // #pragma omp parallel
    {
        // Declara a pilha e o índice superior (top) para controle da pilha
        int stack[w * h];
        int top = -1;

        // Adiciona o ponto inicial à pilha
        stack[++top] = start;

        // Enquanto houver elementos na pilha
        while (top >= 0)
        {
            // Retira o último elemento da pilha (última célula a ser processada)
            int c = stack[top--];

            // Calcula as coordenadas (y, x) a partir do índice linear c
            const int y = c / w, x = c % w;

            // Se a célula já foi marcada como viva, ignora esta iteração
            if (live[c])
                continue;

// Marca a célula atual como viva
#pragma omp atomic write
            live[c] = 1;

            // Verifica as células vizinhas e as adiciona à pilha, se possível
            // Verificação para célula acima (y > 1 para evitar índice fora do limite superior)
            if (y > 1 && board[c - w] != wall && board[c - w * 2] != wall)
                stack[++top] = c - w; // Adiciona a célula acima à pilha, se não for parede

            // Verificação para célula abaixo (y < h - 2 para evitar índice fora do limite inferior)
            if (y < h - 2 && board[c + w] != wall && board[c + w * 2] != wall)
                stack[++top] = c + w; // Adiciona a célula abaixo à pilha, se não for parede

            // Verificação para célula à esquerda (x > 1 para evitar índice fora do limite esquerdo)
            if (x > 1 && board[c - 1] != wall && board[c - 2] != wall)
                stack[++top] = c - 1; // Adiciona a célula à esquerda à pilha, se não for parede

            // Verificação para célula à direita (x < w - 2 para evitar índice fora do limite direito)
            if (x < w - 2 && board[c + 1] != wall && board[c + 2] != wall)
                stack[++top] = c + 1; // Adiciona a célula à direita à pilha, se não for parede
        }
    }
}
// Função para fazer o parsing do tabuleiro a partir de uma string e define as posições iniciais do jogador e das caixas.
state_t *parse_board(const char *s)
{
    // Aloca memória para o tabuleiro (w * h células de tamanho uint8_t)
    board = calloc(w * h, sizeof(uint8_t));
    assert(board); // Verifica se a alocação foi bem-sucedida

    // Aloca memória para as células de objetivo (goals) (w * h células de tamanho uint8_t)
    goals = calloc(w * h, sizeof(uint8_t));
    assert(goals); // Verifica se a alocação foi bem-sucedida

    // Aloca memória para as células vivas (live) (w * h células de tamanho uint8_t)
    live = calloc(w * h, sizeof(uint8_t));
    assert(live); // Verifica se a alocação foi bem-sucedida

    n_boxes = 0; // Inicializa a contagem de caixas

    // Loop para analisar a string que representa o tabuleiro
    for (int i = 0; s[i]; i++)
    {
        switch (s[i]) // Ação baseada no caractere da string
        {
        case '#':            // Se o caractere for '#', marca a célula como parede
            board[i] = wall; // Marca a célula como parede
            continue;        // Continua para o próximo caractere
        case '.':            // Se o caractere for '.', marca a célula como objetivo
        case '+':            // Se o caractere for '+', marca o jogador sobre um objetivo
            goals[i] = 1;    // Marca a célula como objetivo
        case '@':            // Se o caractere for '@', marca o jogador
            continue;        // Continua para o próximo caractere
        case '*':            // Se o caractere for '*', marca a célula como caixa sobre um objetivo
            goals[i] = 1;    // Marca a célula como objetivo
        case '$':            // Se o caractere for '$', marca a célula como caixa
            n_boxes++;       // Incrementa o número de caixas
            continue;        // Continua para o próximo caractere
        default:             // Se o caractere for desconhecido, continua para o próximo
            continue;
        }
    }

    // Alinha o tamanho do estado, considerando a quantidade de caixas
    const int is = sizeof(int);                                                         // Tamanho de um int
    state_size = (sizeof(state_t) + (1 + n_boxes) * sizeof(cidx_t) + is - 1) / is * is; // Ajuste de alinhamento de tamanho

    // Cria o estado inicial usando a função newstate
    state_t *state = newstate(NULL);

// Parâmetros de execução paralela para marcar as células vivas
#pragma omp parallel for
    for (int i = 0; i < w * h; i++)
    {
        if (goals[i]) // Se for uma célula de objetivo
        {
            mark_live_iterative(i); // Marca as células vivas de forma iterativa
        }
    } // Espera todas as tarefas paralelas terminarem

    // Atribui as posições iniciais para o jogador e as caixas
    for (int i = 0, j = 0; i < w * h; i++)
    {
        if (s[i] == '$' || s[i] == '*')
            state->c[++j] = i; // Adiciona a posição da caixa
        else if (s[i] == '@' || s[i] == '+')
            state->c[0] = i; // Adiciona a posição do jogador
    }

    // Retorna o estado inicial
    return state;
}

/*-----------  Tabela Hash -----------*/

// Função para calcular o hash de um estado (hash K&R)
void hash(state_t *s)
{
    if (!s->h)
    {
        register hash_t ha = 0;
        cidx_t *p = s->c;
        for (int i = 0; i <= n_boxes; i++) // Calcula o hash com base nas posições das células
            ha = p[i] + 31 * ha;
        s->h = ha; // Define o hash do estado
    }
}

// Funções de manipulação da tabela hash
state_t **buckets;
hash_t hash_size, fill_limit, filled;

// Reorganiza e expande a tabela hash para suportar mais estados
void extend_table()
{
    int old_size = hash_size;
    if (!old_size)
    {
        hash_size = 1024;
        filled = 0;
        fill_limit = hash_size * 3 / 4;
    }
    else
    {
        hash_size *= 2;
        fill_limit *= 2;
    }

    state_t **new_buckets = calloc(hash_size, sizeof(state_t *));
    assert(new_buckets);

    const hash_t bits = hash_size - 1;

#pragma omp parallel for
    for (int i = 0; i < old_size; i++)
    {
        state_t *head = buckets[i];
        while (head)
        {
            state_t *next = head->next;
            const int j = head->h & bits;
#pragma omp atomic capture
            {
                head->next = new_buckets[j];
                new_buckets[j] = head;
            }
            head = next;
        }
    }

    free(buckets);
    buckets = new_buckets;
}

// Função para procurar um estado na tabela de hash, verifica se um estado já foi explorado usando a tabela hash
state_t *lookup(state_t *s)
{
    hash(s); // Calcula o hash do estado
    state_t *f = buckets[s->h & (hash_size - 1)];

    // Nesse caso, para paralelizar, necessitaria criar uma estrutura a mais para poder realizar o processo em paralelo, além de funções de leitura
    // que no caso para esse teste pequeno, não seria muito eficiente, ou a melhoria de performance seria minima, não compensando a memória extra necessária
    for (; f; f = f->next)
    {
        if (                                                     //(f->h == s->h) &&
            !memcmp(s->c, f->c, sizeof(cidx_t) * (1 + n_boxes))) // Compara os estados
            break;
    }

    return f;
}

// Função para adicionar um estado à tabela de hash
bool add_to_table(state_t *s)
{
    if (lookup(s)) // Se o estado já existe na tabela, retorna falso
    {
        unnewstate(s);
        return false;
    }

    if (filled++ >= fill_limit) // Se a tabela estiver cheia, expande a capacidade da tabela
        extend_table();

    hash_t i = s->h & (hash_size - 1); // Calcula o índice da tabela

    s->next = buckets[i];
    buckets[i] = s; // Adiciona o estado à tabela
    return true;
}

// Função para verificar se o jogo foi ganho (todas as caixas estão nas metas, ou seja, se o jogador ganhou)
bool success(const state_t *s)
{
    for (int i = 1; i <= n_boxes; i++)
        if (!goals[s->c[i]]) // Verifica se todas as caixas estão nas metas
            return false;
    return true;
}

// Função para mover o jogador e as caixas
// Move o jogador e, se necessário, empurra uma caixa. Gera um novo estado correspondente ao movimento
state_t *move_me(state_t *s, const int dy, const int dx)
{
    const int y = s->c[0] / w;
    const int x = s->c[0] % w;
    const int y1 = y + dy;
    const int x1 = x + dx;
    const int c1 = y1 * w + x1;

    if (y1 < 0 || y1 > h || x1 < 0 || x1 > w ||
        board[c1] == wall) // Verifica se o movimento é válido
        return NULL;

    int at_box = 0;
    for (int i = 1; i <= n_boxes; i++)
    {
        if (s->c[i] == c1)
        {
            at_box = i; // Verifica se o jogador está em uma caixa
            break;
        }
    }

    int c2;
    if (at_box) // Se houver uma caixa, move a caixa
    {
        c2 = c1 + dy * w + dx;
        if (board[c2] == wall || !live[c2])
            return NULL;
        for (int i = 1; i <= n_boxes; i++)
            if (s->c[i] == c2) // Verifica se a nova posição da caixa está ocupada
                return NULL;
    }

    state_t *n = newstate(s);                             // Cria um novo estado
    memcpy(n->c + 1, s->c + 1, sizeof(cidx_t) * n_boxes); // Copia a posição das caixas

    cidx_t *p = n->c;
    p[0] = c1; // Atualiza a posição do jogador

    if (at_box)
        p[at_box] = c2; // Atualiza a posição da caixa

    // Ordena as posições das caixas (bubble sort)
    for (int i = n_boxes; --i;)
    {
        cidx_t t = 0;
        for (int j = 1; j < i; j++)
        {
            if (p[j] > p[j + 1])
                t = p[j], p[j] = p[j + 1], p[j + 1] = t;
        }
        if (!t)
            break;
    }

    return n;
}

// Variáveis de controle de níveis e soluções
state_t *next_level, *done;

// Função para adicionar um movimento à fila
// Adiciona um novo estado à fila de exploração, verificando se o jogo foi resolvido
bool queue_move(state_t *s)
{
    if (!s || !add_to_table(s)) // Se o estado não for válido, retorna falso
        return false;

    if (success(s)) // Se o jogo foi ganho, define o estado final
    {
        done = s;
        return true;
    }

    s->qnext = next_level;
    next_level = s; // Adiciona o estado à fila de próximos movimentos
    return false;
}

// Função para realizar um movimento em todas as direções
bool do_move(state_t *s)
{
    return queue_move(move_me(s, 0, 1)) ||  // Move para a direita
           queue_move(move_me(s, 0, -1)) || // Move para a esquerda
           queue_move(move_me(s, -1, 0)) || // Move para cima
           queue_move(move_me(s, 1, 0));    // Move para baixo
}

// Função para exibir os movimentos feitos
void show_moves(const state_t *s, int nextPos)
{

    if (s->prev)                      // Se houver um estado anterior, chama a função recursivamente para exibir os movimentos anteriores
        show_moves(s->prev, s->c[0]); // Exibe os movimentos recursivamente
    if (nextPos == -1)                // Calcula as coordenadas do estado atual (cx, cy) e do próximo movimento (nx, ny)
    {
        printf("\n");
        return;
    }

    // Calcula as coordenadas do estado atual (cx, cy) e do próximo movimento (nx, ny)
    int cx = s->c[0] % w;
    int cy = s->c[0] / w;
    int nx = nextPos % w;
    int ny = nextPos / w;
    int box = 0;
    for (int i = 1; !box && i <= n_boxes; i++)
        box = s->c[i] == nextPos; // Verifica se há uma caixa no próximo movimento
    if (cx < nx)
        printf(box ? "R" : "r"); // Move para a direita
    else if (cx > nx)
        printf(box ? "L" : "l"); // Move para a esquerda
    else if (cy < ny)
        printf(box ? "D" : "d"); // Move para baixo
    else if (cy > ny)
        printf(box ? "U" : "u"); // Move para cima
    else if (1)
    {
        printf("Movimento inválido\n");
        exit(1);
    }
}

int main()
{

    // Variáveis para medir o tempo de execução
    struct timeval start, stop;

    // Representação do tabuleiro como uma string
    const char *boardStr =
        "#######################\n"
        "#. #####......##...####\n"
        "#....#.......$        #\n"
        "#..#...#              #\n"
        "#...##..$$            #\n"
        "######.$$...$$$.#     #\n"
        "#.#. #.#             @#\n"
        "#######################\n";

    // Imprime o tabuleiro no formato de string
    printf("%s\n", boardStr);

    // Inicia a medição do tempo
    gettimeofday(&start, NULL);

    // Determina a largura (w) e altura (h) do tabuleiro a partir da string
    w = 0;
    h = 0;
    for (int i = 0; boardStr[i] != '\0'; i++)
    {
        // Conta o número de linhas no tabuleiro (indicados por '\n')
        if (boardStr[i] == '\n')
        {
            h++; // Incrementa a altura
        }
        // Conta o número de caracteres na primeira linha (largura do tabuleiro)
        else if (h == 0)
        {
            w++; // Incrementa a largura apenas na primeira linha
        }
    }
    w++; // A largura deve ser incrementada por causa do caractere '\0' no final

    // Faz o parsing da string para o estado inicial do tabuleiro
    state_t *s = parse_board(boardStr);
    printf("Tamanho do mapa: %d x %d\n", w, h);

    // Expande a tabela de hash se necessário
    extend_table();
    // Adiciona o estado inicial à fila de movimentos
    queue_move(s);

    // Enquanto o jogo não for resolvido, continua tentando encontrar a solução
    while (!done) // Enquanto não tiver terminado
    {
        state_t *head = next_level;
        // Itera sobre os estados na próxima camada
        for (next_level = NULL; head && !done; head = head->qnext)
            do_move(head); // Realiza um movimento no estado atual

        // Se não houver mais estados para explorar, significa que não há solução
        if (!next_level)
        {
            puts("Sem solução");
            return 1; // Retorna com erro se não houver solução
        }
    }

    // Imprime os movimentos que levaram à solução
    printf("\nMovimentos: \n");
    show_moves(done, -1); // Mostra a sequência de movimentos

    // Libera a memória alocada para as estruturas de dados
    free(buckets); // Libera a tabela de hash
    free(board);   // Libera o tabuleiro
    free(goals);   // Libera os objetivos
    free(live);    // Libera a lista de estados vivos

    // Libera a memória de blocos encadeados
    while (block_root)
    {
        void *tmp = block_root->next;
        free(block_root); // Libera o bloco atual
        block_root = tmp; // Avança para o próximo bloco
    }

    // Finaliza a medição do tempo
    gettimeofday(&stop, NULL);
    // Calcula o tempo total gasto em milissegundos
    double tempo =
        (((double)(stop.tv_sec) * 1000.0 + (double)(stop.tv_usec / 1000.0)) -
         ((double)(start.tv_sec) * 1000.0 + (double)(start.tv_usec / 1000.0)));
    // Exibe o tempo total de execução
    fprintf(stdout, "Tempo total gasto = %g ms\n", tempo);

    return 0;
}

/*
Input para teste rapido:

#######
#     #
#     #
#. #  #
#. $$ #
#.$$  #
#.#  @#
#######


Output:

ulULLulDDurrrddlULrruLLrrUruLLLulD

Teste mais lento

const char *boardStr =
        "#######################\n"
        "#. #####......##...####\n"
        "#....#.......$        #\n"
        "#..#...#              #\n"
        "#...##..$$            #\n"
        "######.$$...$$$.#     #\n"
        "#.#. #.#             @#\n"
        "#######################\n";

Movimentos:
llllllllURuLdLUUrUdllllDLrrddllULrUU

*/