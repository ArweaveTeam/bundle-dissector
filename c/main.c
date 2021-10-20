#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "jsmn.h"


// The maximum size of single chunk
// https://github.com/ArweaveTeam/arweave/blob/a897b8cce6e93038625866f053d5cba07701c30c/apps/arweave/include/ar.hrl#L330-L331
#define MAX_CHUNK_SIZE 262144



struct ArweaveNode {
  char domain[256];
  int port;
  struct hostent *host;
};

struct ArweaveBundle {
  char tx_id[256];
  uint64_t endOffset;
  uint64_t startOffset;
  uint64_t currentOffset;
  uint64_t size;
};

struct ArweaveDataItemInfo {
  int index;
  char tx_id[256];
  uint64_t startOffset;
  uint64_t endOffset;
};

struct ArweaveBundleHeader {
  uint32_t data_item_cnt;
  struct ArweaveDataItemInfo *offsets[];
};

struct StateMachine {
  int iter_index;
  int chunk_buffer_index;
  int di_cnt_done;
  int offset_done;
  int header_done;
};

static const uint8_t base64urlDecTable[128] =  {
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3E, 0xFF, 0xFF,
  0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
  0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F,
  0xFF, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
  0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static int jsoneq(const char *json, jsmntok_t *tok, const char *s) {
  if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
      strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
    return 0;
  }
  return -1;
}

uint64_t strToLong(const char *buff) {
  const char *s = buff;
  // consume leading whitespace
  while (isspace((unsigned char)*s)) {
    s++;
  }
  int sign = *s;
  if (sign == '-' || sign == '+') {
    s++;
  }
  // Now code knows it the text is "negative"

  // rest of OP's code needs a some work
  char *end;
  errno = 0;
  unsigned long long sl = strtoull(s, &end, 10);

  if (end == s) {
    fprintf(stderr, "%s: not a decimal number\n", buff);
  } else if ('\0' != *end) {
    fprintf(stderr, "%s: extra characters at end of input: %s\n", buff, end);
    // } else if ((sl < 0 || ULONG_MAX == sl) && ERANGE == errno) {
  } else if (sign == '-') {
    fprintf(stderr, "%s negative\n", buff);
    sl = 0;
    errno = ERANGE;
  } else if (ERANGE == errno) {
    fprintf(stderr, "%s out of range of type uint64_t\n", buff);

  }
  return (uint64_t)sl;
}

int ReadHttpStatus(int sock) {
  char c;
  char buff[1024] = "";
  char *ptr = buff + 1;
  int bytes_received;
  int status;

  printf("Begin Response ..\n");
  while ((bytes_received = recv(sock, ptr, 1, 0))) {
    if (bytes_received == -1) {
      perror("ReadHttpStatus");
      exit(1);
    }

    if ((ptr[-1] == '\r') && (*ptr == '\n'))
      break;
    ptr++;
  }
  *ptr = 0;
  ptr = buff + 1;

  sscanf(ptr, "%*s %d ", &status);

  printf("%s\n", ptr);
  printf("status=%d\n", status);
  printf("End Response ..\n");
  return (bytes_received > 0) ? status : 0;
}

int ParseHeader(int sock) {
  char c;
  char buff[1024] = "";
  char *ptr = buff + 4;
  int bytes_received;
  int status;

  printf("Begin HEADER ..\n");
  while ((bytes_received = recv(sock, ptr, 1, 0))) {
    if (bytes_received == -1) {
      perror("Parse Header");
      exit(1);
    }

    if ((ptr[-3] == '\r') && (ptr[-2] == '\n') && (ptr[-1] == '\r') &&
        (*ptr == '\n')) {
      break;
    }
    ptr++;
  }

  *ptr = 0;
  ptr = buff + 4;
  // printf("%s",ptr);

  if (bytes_received) {

    ptr = strstr(ptr, "content-length:");

    /* if (ptr == NULL) { */
    /*   strstr(ptr, "content-length:"); */
    /* } */

    if (ptr) {
      sscanf(ptr, "%*s %d", &bytes_received);
    } else {
      bytes_received = -1; // unknown size
    }

    printf("Content-Length: %d\n", bytes_received);
  }
  printf("End HEADER ..\n");
  return bytes_received;
}

/**
 * @brief Base64url decoding algorithm
 * @param[in] input Base64url-encoded string
 * @param[in] inputLen Length of the encoded string
 * @param[out] output Resulting decoded data
 * @param[out] outputLen Length of the decoded data
 * @return Error code
 **/
// https://www.oryx-embedded.com/doc/base64url_8c_source.html
int base64urlDecode(const char *input, int inputLen, char *output, int *outputLen) {
  int error = 0;
  int value;
  int c;
  int i;
  int n;
  uint8_t *ptr;

  //Check the length of the input string
  if((inputLen % 4) == 1) {
    printf("invalid base64url length of a given chunk\n");
    return 0;
  }

  if(input == NULL && inputLen != 0) {
    printf("(base64urlDecode) invalid input params\n");
  }

  if(outputLen == NULL) {
    printf("(base64urlDecode) invalid output params\n");
  }


  //Initialize status code
  error = 1;

  //Point to the buffer where to write the decoded data
  ptr = (uint8_t *) output;

  //Initialize variables
  n = 0;
  value = 0;

  //Process the Base64url-encoded string
  for(i = 0; i < inputLen && error == 1; i++) {
    //Get current character
    c = (char) input[i];

    //Check the value of the current character
    if(c < 128 && base64urlDecTable[c] < 64) {
      //Decode the current character
      value = (value << 6) | base64urlDecTable[c];

      //Divide the input stream into blocks of 4 characters
      if((i % 4) == 3) {
        //Map each 4-character block to 3 bytes
        if(output != NULL)
          {
            ptr[n] = (value >> 16) & 0xFF;
            ptr[n + 1] = (value >> 8) & 0xFF;
            ptr[n + 2] = value & 0xFF;
          }
        if (n < 32) {
          printf("n: %d %d %d\n", n, ptr[n], (uint32_t) ((uint8_t) ptr[n]));
        }
        //Adjust the length of the decoded data
        n += 3;
        //Decode next block
        value = 0;
      }
    }
    else {
      //Implementations must reject the encoded data if it contains
      //characters outside the base alphabet
      printf("invalid base64url char: '%i'\n", (int) c);
      error = 0;
    }
  }

  //Check status code
  if(!error) {
    //All trailing pad characters are omitted in Base64url
    if((inputLen % 4) == 2) {
      //The last block contains only 1 byte
      if(output != NULL) {
        //Decode the last byte
        ptr[n] = (value >> 4) & 0xFF;
      }

      //Adjust the length of the decoded data
      n++;
    }
    else if((inputLen % 4) == 3) {
      //The last block contains only 2 bytes
      if(output != NULL) {
        //Decode the last two bytes
        ptr[n] = (value >> 10) & 0xFF;
        ptr[n + 1] = (value >> 2) & 0xFF;
      }

      //Adjust the length of the decoded data
      n += 2;
    }
    else {
      //No pad characters in this case
    }
  }

  //Total number of bytes that have been written
  /* printf("n %zi error %i inputlen %zu\n", n, error, inputLen); */
  *outputLen = n;

  //Return status code
  return error;
}

int ProcessChunk(struct ArweaveNode *arNode,
                 struct ArweaveBundle *arBundle,
                 struct ArweaveBundleHeader *arBundleHeader,
                 struct StateMachine *state,
                 int encChunkSize,
                 char* chunk_buffer) {
  /* printf("\n\%s\n", chunk_buffer); */
  /* int cnk_index = 0; */
  printf("z-1 \n");
  char buffer[MAX_CHUNK_SIZE * 4];
  int decodedCnt = 0;
  int thisCnt;
  /* int  bufferIdx = 0; */
  /* char buffer1[1024]; */
  /* char buffer2[1024 * 4]; */
  /* int  buffer2idx = 0; */
  /* [state->chunk_buffer_index] */
  /* printf("zero %c \n", chunk_buffer[0]); */

  if (!base64urlDecode(chunk_buffer, encChunkSize, buffer, &thisCnt)) {
    printf("Decode Failure\n");
    exit(1);
    return 1;
  }

  /* printf("type: %d %x\n", (uint8_t) buffer[0]); */
  /* printf("one %s %d %d\n", buffer, thisCnt, encChunkSize); */
  /* printf("bytes: %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n", buffer[0* sizeof(uint8_t)], buffer[1* sizeof(uint8_t)], buffer[2* sizeof(uint8_t)], buffer[3* sizeof(uint8_t)], buffer[4* sizeof(uint8_t)], buffer[5* sizeof(uint8_t)], buffer[6* sizeof(uint8_t)], buffer[7* sizeof(uint8_t)], buffer[8* sizeof(uint8_t)], buffer[9* sizeof(uint8_t)], buffer[10* sizeof(uint8_t)], buffer[11* sizeof(uint8_t)], buffer[12* sizeof(uint8_t)], */
  /*        buffer[13* sizeof(uint8_t)], buffer[14* sizeof(uint8_t)], buffer[15* sizeof(uint8_t)], buffer[16* sizeof(uint8_t)], buffer[17* sizeof(uint8_t)], buffer[18* sizeof(uint8_t)], buffer[19* sizeof(uint8_t)], buffer[20* sizeof(uint8_t)], buffer[21* sizeof(uint8_t)], buffer[22* sizeof(uint8_t)], buffer[23* sizeof(uint8_t)], buffer[24* sizeof(uint8_t)], buffer[25* sizeof(uint8_t)]); */
  state->chunk_buffer_index += thisCnt;

  if (state->di_cnt_done != 1 && state->chunk_buffer_index > 32) {
    for(int i=0; i < 32; i++) {

      printf("i: %d val: %hhu %lu\n", i, (uint8_t) buffer[i * sizeof(uint8_t)], sizeof(uint8_t));

      /* arBundleHeader->data_item_cnt += (uint8_t) &buffer[i * sizeof(uint8_t)]; */

      /* arBundleHeader->data_item_cnt += strtol((uint8_t) &buffer[i * sizeof(uint8_t)], 0, 16); */
      /* arBundleHeader->data_item_cnt <<= 4; */
      /* arBundleHeader->data_item_cnt |= (uint32_t) buffer[i]; */
    }

    /* arBundleHeader->data_item_cnt = 0; */
    /* char dataItemCnt[3] = { 0 }; */
    /* sprintf(dataItemCnt, "%2x", buffer[0]); */
    /* arBundleHeader->data_item_cnt = strtol(dataItemCnt, 0, 16); */
    /* arBundleHeader->data_item_cnt */
    printf("333 %d \n", arBundleHeader->data_item_cnt);
    /* arBundle->data_item_cnt */

    /* for(int i=7; i>=0; i--) { */
    /*   arBundleHeader->data_item_cnt <<= 4; */
    /*   arBundleHeader->data_item_cnt |= (uint32_t) buffer[i]; */
    /* } */
    /* printf("data_item_cnt %d\n", arBundleHeader->data_item_cnt); */
    state->di_cnt_done = 1;
  }

  return 0;

}

int ProcessBundle(struct ArweaveNode *arNode,
                  struct ArweaveBundle *arBundle,
                  struct ArweaveBundleHeader *arBundleHeader,
                  struct StateMachine *state) {

  int sock, contentlengh, status;

  // sizeof return annoying long int which we dont need
  int chunk_buffer_len = MAX_CHUNK_SIZE * 4;
  int page_buffer_len = MAX_CHUNK_SIZE * 8;

  struct sockaddr_in server_addr;
  char chunk_buffer[chunk_buffer_len];
  char page_buffer[page_buffer_len];
  char send_data[1024];
  char recv_data[1024];
  char path[256];

  // very crude jq for .chunk
  const char *chunk_token = "\"chunk\"";
  int json_token_chunk_start = -1;
  int current_chunk_start = -1;
  int current_chunk_end = -1;
  int current_page_index = 0;
  int bytes_received;

  state->chunk_buffer_index = 0;
  state->iter_index = 0;
  state->di_cnt_done = -1;
  state->offset_done = -1;
  state->header_done = -1;

  while (arBundle->currentOffset < arBundle->endOffset) {

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
      perror("Socket");
      exit(1);
    }
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(arNode->port);
    server_addr.sin_addr = *((struct in_addr *)arNode->host->h_addr);
    bzero(&(server_addr.sin_zero), 8);

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1) {
      perror("Connect");
      exit(1);
    }

    sprintf(path, "chunk/%" PRId64, arBundle->currentOffset);
    snprintf(send_data, sizeof(send_data), "GET /%s HTTP/1.1\r\nHost: %s\r\n\r\n",
             path, arNode->domain);

    if (send(sock, send_data, strlen(send_data), 0) == -1) {
      perror("send");
      exit(2);
    }

    status = ReadHttpStatus(sock);
    if (status >= 400 && status < 500) {
      fprintf(stderr, "chunk offset %" PRId64 "wasn't found\n", arBundle->currentOffset);
      exit(EXIT_FAILURE);
    }
    contentlengh = ParseHeader(sock);

    while ((bytes_received = recv(sock, recv_data, 1024, 0))) {
      printf("%d bytes received\n", bytes_received);
      printf("%d total bytes received\n", current_page_index);

      if (bytes_received == -1) {
        perror("recieve");
        exit(3);
      }
      memcpy(page_buffer + current_page_index, recv_data, bytes_received);

      if (json_token_chunk_start < 0 || current_chunk_start < 0 || current_chunk_end < 0) {
        for (int c = 0; c < bytes_received; c++) {
          if (json_token_chunk_start < 0) {
            if (strncmp(chunk_token, &recv_data[c], 7) == 0) {
              json_token_chunk_start = current_page_index + c;
            }
          } else if (((7 + current_page_index - current_chunk_start) > c) && current_chunk_start < 0) {
            if (recv_data[c] == '"' ) {
              current_chunk_start = current_page_index + c + 3;
            }
          } else if (((1 + current_page_index - current_chunk_start) > c) && current_chunk_end < 0) {
            if (recv_data[c] == '"' ) {
              current_chunk_end = current_page_index + c;
            }
          }
        }
      }

      if (json_token_chunk_start > -1 && current_chunk_start > -1 && current_chunk_end > -1) {
        int encChunkSize = current_chunk_end - current_chunk_start;
        if (((encChunkSize + state->chunk_buffer_index) % page_buffer_len) == (encChunkSize + state->chunk_buffer_index)) {
          memcpy(chunk_buffer + state->chunk_buffer_index, page_buffer + current_chunk_start, encChunkSize);
        } else {
          printf("not same size %d %d\n", ((encChunkSize + state->chunk_buffer_index) % page_buffer_len), (encChunkSize + state->chunk_buffer_index));
          memcpy(chunk_buffer + state->chunk_buffer_index, page_buffer + current_chunk_start,
                 page_buffer_len - state->chunk_buffer_index);
          memcpy(chunk_buffer, &page_buffer[current_chunk_start],
                 (encChunkSize + state->chunk_buffer_index) % page_buffer_len);
        }
        printf("chunk buffer %s \n first char %c \n", chunk_buffer, chunk_buffer[0]);
        ProcessChunk(arNode, arBundle, arBundleHeader, state, encChunkSize, chunk_buffer);
      }

      if (current_page_index >= contentlengh ||
          (json_token_chunk_start > -1 && current_chunk_start > -1 && current_chunk_end > -1)) {
        close(sock);
        json_token_chunk_start = -1;
        current_chunk_start = -1;
        current_chunk_end = -1;
        current_page_index = 0;
        break;
      } else {
        current_page_index += bytes_received;
      }

      printf("json_token_chunk_start: %d current_chunk_start: %d current_chunk_end: %d\n",
             json_token_chunk_start, current_chunk_start, current_chunk_end);
    }
    exit(0);
    break;
  }


  return 0;

}

int GetOffsetAndSize(struct ArweaveNode *arNode,
                     struct ArweaveBundle *arBundle) {

  int sock;
  struct sockaddr_in server_addr;
  jsmn_parser parser;
  jsmntok_t tokens[2048];
  int parseResult;
  /* int bytes_received; */
  char send_data[1024];
  char recv_data[1024];
  char path[256] = "tx/";

  strcat(path, arBundle->tx_id);
  strcat(path, "/offset");
  printf("path: %s\n", path);

  if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    perror("Socket");
    exit(1);
  }
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(arNode->port);
  server_addr.sin_addr = *((struct in_addr *)arNode->host->h_addr);
  bzero(&(server_addr.sin_zero), 8);

  if (connect(sock, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) ==
      -1) {
    perror("Connect");
    exit(1);
  }

  snprintf(send_data, sizeof(send_data), "GET /%s HTTP/1.1\r\nHost: %s\r\n\r\n",
           path, arNode->domain);

  if (send(sock, send_data, strlen(send_data), 0) == -1) {
    perror("send");
    exit(2);
  }

  int contentlengh;
  int status;
  char *body;

  if ((status = ReadHttpStatus(sock)) && (contentlengh = ParseHeader(sock))) {

    if (status == 0) {
      fprintf(stderr, "Fatal network error\n");
      exit(EXIT_FAILURE);
    }

    if (status >= 400 && status < 500) {
      fprintf(stderr, "tx %s wasn't found\n", arBundle->tx_id);
      exit(EXIT_FAILURE);
    }

    if (status != 200) {
      fprintf(stderr, "tx %s couldn't be fetched from %s\n", arBundle->tx_id,
              arNode->domain);
      exit(EXIT_FAILURE);
    }

    int bytes_received;
    int bytes = 0;
    body = (char *)malloc((contentlengh + 1) * sizeof(char));

    /* FILE *fd = fopen("test.png", "wb"); */
    printf("Saving data...\n\n");

    while ((bytes_received = recv(sock, recv_data, 1024, 0))) {
      if (bytes_received == -1) {
        perror("recieve");
        exit(3);
      }

      /* body[bytes] = recv_data; */
      strcpy((char *)body, recv_data);

      /* fwrite(recv_data, 1, bytes_received, fd); */
      bytes += bytes_received;
      printf("Bytes recieved: %d from %d\n", bytes, contentlengh);
      if (bytes == contentlengh) {
        break;
      }
    }
  }

  jsmn_init(&parser);
  parseResult = jsmn_parse(&parser, body, strlen(body), tokens, 2048);

  if (parseResult < 0) {
    fprintf(stderr, "Failed to parse JSON: %d\n", parseResult);
    exit(1);
  }

  for (int i = 1; i < parseResult; i++) {
    if (jsoneq(body, &tokens[i], "size") == 0) {
      printf("size: %s\n", strndup(body + tokens[i + 1].start, tokens[i + 1].end - tokens[i + 1].start));
      /* printf("dim: start %d end %d size %d\n", tokens[i + 1].start, tokens[i + 1].end, tokens[i + 1].size); */
      arBundle->size = strToLong(strndup(body + tokens[i + 1].start, tokens[i + 1].end - tokens[i + 1].start));
    }
    if (jsoneq(body, &tokens[i], "offset") == 0) {
      printf("offset: %s\n", strndup(body + tokens[i + 1].start, tokens[i + 1].end - tokens[i + 1].start));
      arBundle->endOffset = strToLong(strndup(body + tokens[i + 1].start, tokens[i + 1].end - tokens[i + 1].start));
    }
    i++;
  }

  if (arBundle->endOffset < 0) {
    fprintf(stderr, "key 'size' not found in json /offset response\n");
    exit(1);
  }

  if (arBundle->size < 0) {
    fprintf(stderr, "key 'offset' not found in json /offset response\n");
    exit(1);
  }

  arBundle->startOffset = arBundle->endOffset - arBundle->size + 1;
  arBundle->currentOffset = arBundle->startOffset;

  /* printf("BODY: %s\n", body); */
  /* printf("SIZe: %" PRId64 "\n", arBundle->size); */
  /* printf("SIZe: %" PRId64 "\n", arBundle->offset); */

  close(sock);

  return 0;
}

int main(int argc, char *argv[]) {

  int optc;
  int optarg_end = 0;

  int customPort = 0;
  char customPortStr[64];
  /* char tx[256]; */
  struct ArweaveNode arNode;
  struct ArweaveBundle arBundle;
  struct ArweaveBundleHeader arBundleHeader;
  struct StateMachine state;

  while (optarg_end == 0) {

    int option_index = 0;

    static struct option cli_options[] = {{"node", required_argument, 0, 'n'},
                                          {"tx", required_argument, 0, 't'},
                                          {"port", optional_argument, 0, 'p'},
                                          {NULL, 0, 0, '\0'}};

    optc = getopt_long(argc, argv, "n:t:p:", cli_options, &option_index);

    if (optc == -1) {
      optarg_end = 1;
      /* printf("bad argc %s\n\n", arNode.domain); */
      break;
    }

    switch (optc) {
    case 'n':
      strcpy(arNode.domain, optarg);
      break;

    case 't':
      strcpy(arBundle.tx_id, optarg);
      break;
    case 'p':
      strcpy(customPortStr, optarg);
      customPort = 1;
      break;

    case '?':
      break;

    default:
      fprintf(stderr,
              "Usage: %s --node ARWEAVE_NODE_URL --port ARWEAVE_NODE_PORT --tx "
              "ARWEAVE_BUNDLE_TX_ID\n",
              argv[0]);
      return EXIT_FAILURE;
    }
  }

  if (strlen(arNode.domain) == 0 || strlen(arBundle.tx_id) == 0) {
    fprintf(stderr,
            "Usage: %s --node ARWEAVE_NODE_URL --port ARWEAVE_NODE_PORT --tx "
            "ARWEAVE_BUNDLE_TX_ID\n",
            argv[0]);
    return EXIT_FAILURE;
  }

  if (customPort == 0) {
    arNode.port = 1984;
  }

  printf("getting host by name \n");

  arNode.host = gethostbyname(arNode.domain);

  if (arNode.host == NULL) {
    herror("gethostbyname");
    exit(1);
  }

  printf("getting offset and size \n");

  GetOffsetAndSize(&arNode, &arBundle);

  ProcessBundle(&arNode, &arBundle, &arBundleHeader, &state);

  /*
  char domain[] = "sstatic.net";
  char path[]="stackexchange/img/logos/so/so-logo-med.png";

  int sock;
  int bytes_received;
  char send_data[1024];
  char recv_data[1024];
  char *p;

  struct sockaddr_in server_addr;
  struct hostent *he;

  arDrive.host = gethostbyname(domain);

  he = gethostbyname(domain);
  if (he == NULL) {
    herror("gethostbyname");
    exit(1);
  }

  if ((sock = socket(AF_INET, SOCK_STREAM, 0))== -1) {
    perror("Socket");
    exit(1);
  }
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(80);
  server_addr.sin_addr = *((struct in_addr *)he->h_addr);
  bzero(&(server_addr.sin_zero),8);

  printf("Connecting ...\n");
  if (connect(sock, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) ==
  -1) { perror("Connect"); exit(1);
  }

  printf("Sending data ...\n");

  snprintf(send_data, sizeof(send_data), "GET /%s HTTP/1.1\r\nHost: %s\r\n\r\n",
  path, domain);

  if (send(sock, send_data, strlen(send_data), 0) == -1) {
    perror("send");
    exit(2);
  }
  printf("Data sent.\n");

  //fp=fopen("received_file","wb");
  printf("Recieving data...\n\n");

  int contentlengh;

  if (ReadHttpStatus(sock) && (contentlengh=ParseHeader(sock))) {

    int bytes=0;
    FILE* fd=fopen("test.png","wb");
    printf("Saving data...\n\n");

    while ((bytes_received = recv(sock, recv_data, 1024, 0))) {
      if(bytes_received==-1) {
        perror("recieve");
        exit(3);
      }


      fwrite(recv_data,1,bytes_received,fd);
      bytes+=bytes_received;
      printf("Bytes recieved: %d from %d\n",bytes,contentlengh);
      if(bytes==contentlengh)
        break;
    }
    fclose(fd);
  }



  close(sock);
  printf("\n\nDone.\n\n");
  return 0;
  */
  return 0;
}
