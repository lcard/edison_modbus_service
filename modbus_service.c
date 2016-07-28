/*
 * A MODBUS service which exposes the Intel Edison for Arduino board's
 * 	digital io
 *	analog inputs
 * 	PWM outputs
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <string.h>

#include <modbus/modbus.h>
#include <mraa.h>

#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define NB_CONNECTION 10

#define N_INREGS 6
#define N_HREGS  4
#define N_DISCRETES 5
#define N_COILS 5

modbus_t *ctx = NULL;
int server_socket = -1;
modbus_mapping_t *mb_mapping;

typedef struct edison_gpio {
        mraa_gpio_context gpio;
        int pin;
} edison_gpio;

typedef struct edison_pwm {
        mraa_pwm_context pwm;
        int pin;
} edison_pwm;

typedef struct edison_aio {
        mraa_aio_context aio;
        int pin;
} edison_aio;

typedef struct edison {
        edison_gpio inputs[N_DISCRETES];
        edison_gpio outputs[N_COILS];
        edison_pwm  pwms[N_HREGS];
        edison_aio  aios[N_INREGS];
} edison;

edison e;

void modbus_mapping_dump(modbus_mapping_t * m){

	printf("nb_bits %i\n", m->nb_bits);
	printf("nb_input_bits %i\n", m->nb_input_bits);
	printf("nb_input_registers %i\n", m->nb_input_registers);
	printf("nb_registers %i\n", m->nb_registers);

	for(int i=0; i<N_COILS; i++) {
		printf("%u ", m->tab_bits[i]);	
	}
	printf("\n");

	for(int i=0; i<N_DISCRETES; i++) {
		printf("%u ", m->tab_input_bits[i]);
	}
	printf("\n");

        for(int i=0; i<N_HREGS; i++) {
		printf("%u ", m->tab_registers[i]);
	}
	printf("\n");

	for(int i=0; i<N_INREGS; i++) {
		printf("%u ", m->tab_input_registers[i]);
	}
	printf("\n");

}

void modbus_query_dump(uint8_t * q){
	
	printf("Transaction ID: %x %x\n", q[0], q[1]);
	printf("Protocol ID: %x %x\n", q[2], q[3]);
	printf("Length: %x %x\n", q[4], q[5]);
	printf("Unit ID: %i\n", q[6]);
	printf("Function Code: %i\n",  q[7]);

	printf("Query: ");
	for (int i=0; i<MODBUS_TCP_MAX_ADU_LENGTH; i++) {
		printf("%3x", q[i]);
        }
	printf("\n");
}

static void close_sigint(int dummy){

        if (server_socket != -1) {
                close(server_socket);
        }
        modbus_free(ctx);
        modbus_mapping_free(mb_mapping);

        for (int i=0; i<N_DISCRETES; i++) {
                mraa_gpio_close(e.inputs[i].gpio);
        }

        for (int i=0; i<N_COILS; i++) {
                mraa_gpio_close(e.outputs[i].gpio);
        }

        for (int i=0; i<N_HREGS; i++) {
                mraa_pwm_close(e.pwms[i].pwm);
        }

        for (int i=0; i<N_INREGS; i++) {
                mraa_aio_close(e.aios[i].aio);
        }

        exit(dummy);
}

/*
 * The Intel Edison for Arduino board has:
 * - 6 analog inputs to map to input registers 0-5 (or 1-6?)
 * - 4 pwm outputs to map to holding registers 0-3 (or 1-4?) pins labeled ~3, ~5, ~6, ~9
 * - 10 remaining digital IO:
 *      - 5 inputs (GPIO 0,1,2,4,7) to be mapped to discretes at address 0 (or 1?), bits 0,1,2,4,7.
 *      - 5 output (GPIO 8,10,11,12,13) to be mapped to coils at address 0 (or 1?), bits 1,3,4,5,6.
*/
void edison_io_init(edison * e){

	e->inputs[0].pin = 0;
	e->inputs[1].pin = 1;
	e->inputs[2].pin = 2;
	e->inputs[3].pin = 4;
	e->inputs[4].pin = 7;

	e->outputs[0].pin = 8;
	e->outputs[1].pin = 10;
	e->outputs[2].pin = 11;
	e->outputs[3].pin = 12;
	e->outputs[4].pin = 13;

	e->pwms[0].pin = 3;
	e->pwms[1].pin = 5;
	e->pwms[2].pin = 6;
	e->pwms[3].pin = 9;

	e->aios[0].pin = 0;
	e->aios[1].pin = 1;
	e->aios[2].pin = 2;
	e->aios[3].pin = 3;
	e->aios[4].pin = 4;
	e->aios[5].pin = 5;

	for (int i=0; i<N_DISCRETES; i++) {
		e->inputs[i].gpio = mraa_gpio_init(e->inputs[i].pin);
		if (NULL == e->inputs[i].gpio){
			printf("Failed to init input %i on pin %i.\n", i, e->inputs[i].pin);
			printf("Note: App requires root priviledge for access to platform io.\n");
			exit(1);
		} else
		mraa_gpio_dir(e->inputs[i].gpio, MRAA_GPIO_IN);
	}

	for (int i=0; i<N_COILS; i++) {
		e->outputs[i].gpio = mraa_gpio_init(e->outputs[i].pin);
		if (NULL == e->outputs[i].gpio){
		printf("Failed to init output.\n");
			exit(1);
		}
		mraa_gpio_dir(e->outputs[i].gpio, MRAA_GPIO_OUT);
	}

	for (int i=0; i<N_HREGS; i++) {
		e->pwms[i].pwm = mraa_pwm_init(e->pwms[i].pin);
		if (NULL == e->pwms[i].pwm){
			printf("Failed to init pwm %i on pin %i.\n", i, e->pwms[i].pin);
			exit(1);
		}
		mraa_pwm_period_us(e->pwms[i].pwm, mraa_pwm_get_max_period(e->pwms[i].pwm));
		mraa_pwm_enable(e->pwms[i].pwm,1);
	}

	for (int i=0; i<N_INREGS; i++) {
		e->aios[i].aio = mraa_aio_init(e->aios[i].pin);
		if (NULL == e->aios[i].aio){
			printf("Failed to init aio.\n");
			exit(1);
		}
		mraa_aio_set_bit(e->aios[i].aio, 12);
	}
}

void update_map_from_coils(edison * e, modbus_mapping_t * m){
	//printf("Updating map from coils (digital outputs).\n");

	for(int i=0; i<N_COILS; i++){
		m->tab_bits[i] = mraa_gpio_read(e->outputs[i].gpio);
	}
}

void update_map_from_discretes(edison * e, modbus_mapping_t * m){
	//printf("Updating map from discretes (digital inputs).\n");

	for(int i=0; i<N_DISCRETES; i++){
                m->tab_input_bits[i] = (uint8_t) mraa_gpio_read(e->inputs[i].gpio);
        }
}

void update_map_from_hregs(edison * e, modbus_mapping_t * m){
	//printf("Updating map from holding registers (PWMs).\n");

	for(int i=0; i<N_HREGS; i++){
                m->tab_registers[i] = (1000.0 * mraa_pwm_read(e->pwms[i].pwm));
        }
}

void update_map_from_inregs(edison * e, modbus_mapping_t * m){
	//printf("Updating map from input registers (analog inputs).\n");

	for(int i=0; i<N_INREGS; i++){
                m->tab_input_registers[i] = (uint16_t) mraa_aio_read(e->aios[i].aio);
        }
}

void update_coils_from_map(edison * e, modbus_mapping_t * m){
	//printf("Updating coils (digital outputs) from map.\n");

	for(int i=0; i<N_COILS; i++){
		mraa_gpio_write(e->outputs[i].gpio, m->tab_bits[i]);
        }
}

void update_hregs_from_map(edison * e, modbus_mapping_t * m){
	//printf("Updating holding registers from \n");

        for(int i=0; i<N_HREGS; i++){
                mraa_pwm_write(e->pwms[i].pwm,(m->tab_registers[i] / 1000.0));
        }
}

int main(void) {

	uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];
	int master_socket;
	int rc;
	fd_set refset;
 	fd_set rdset;
	int fdmax;

	edison_io_init(&e);

 	ctx = modbus_new_tcp("0.0.0.0", MODBUS_TCP_DEFAULT_PORT); 

 	mb_mapping = modbus_mapping_new(N_COILS, N_DISCRETES, N_HREGS, N_INREGS);
	if (mb_mapping == NULL) {
		fprintf(stderr, "Failed to allocate Modbus mapping: %s\n",
		modbus_strerror(errno));
		modbus_free(ctx);
 		return -1;
	}

	server_socket = modbus_tcp_listen(ctx, NB_CONNECTION);

	signal(SIGINT, close_sigint);

	/* Clear the reference set of socket */
	FD_ZERO(&refset);
	/* Add the server socket */
	FD_SET(server_socket, &refset);

	/* Keep track of the max file descriptor */
	fdmax = server_socket;

	for (;;) {
		rdset = refset;
		if (select(fdmax+1, &rdset, NULL, NULL, NULL) == -1) {
 			perror("Server select() failure.");
 			close_sigint(1);
		}

		/* Run through the existing connections looking for data to be read. */
		for (master_socket = 0; master_socket <= fdmax; master_socket++) {

			if (!FD_ISSET(master_socket, &rdset)) {
				continue;
			}

			if (master_socket == server_socket) {
				/* A client is asking a new connection */
				socklen_t addrlen;
				struct sockaddr_in clientaddr;
				int newfd;

				/* Handle new connections */
				addrlen = sizeof(clientaddr);
				memset(&clientaddr, 0, sizeof(clientaddr));
				newfd = accept(server_socket, (struct sockaddr *)&clientaddr, &addrlen);
				if (newfd == -1) {
					perror("Server accept() error");
				} else {
					FD_SET(newfd, &refset);

					if (newfd > fdmax) {
						/* Keep track of the maximum */
						fdmax = newfd;
					}

					printf("New connection from %s:%d on socket %d\n",
					inet_ntoa(clientaddr.sin_addr), clientaddr.sin_port, newfd);
				}
			} else {
				modbus_set_socket(ctx, master_socket);
				rc = modbus_receive(ctx, query);
				if (rc > 0) {

					//modbus_query_dump(query);

					uint8_t fc_code = query[7]; 

					switch(fc_code) {
						case MODBUS_FC_READ_COILS: 		update_map_from_coils(&e, mb_mapping); 		break;
						case MODBUS_FC_READ_DISCRETE_INPUTS: 	update_map_from_discretes(&e, mb_mapping);	break;
						case MODBUS_FC_READ_HOLDING_REGISTERS:	update_map_from_hregs(&e, mb_mapping);		break;
						case MODBUS_FC_READ_INPUT_REGISTERS:	update_map_from_inregs(&e, mb_mapping);		break;
					}
					//printf("Sending reply.\n");
					modbus_reply(ctx, query, rc, mb_mapping);

					switch(fc_code) {
						case MODBUS_FC_WRITE_SINGLE_COIL:
						case MODBUS_FC_WRITE_MULTIPLE_COILS:		update_coils_from_map(&e, mb_mapping); break;
						case MODBUS_FC_WRITE_SINGLE_REGISTER:
						case MODBUS_FC_WRITE_MULTIPLE_REGISTERS:	update_hregs_from_map(&e, mb_mapping); break;	
					}
					//printf("End of transaction.\n");

				} else if (rc == -1) {
					/* This example server in ended on connection closing or any errors. */
					//printf("Connection closed on socket %d\n", master_socket);
					close(master_socket);

					/* Remove from reference set */
					FD_CLR(master_socket, &refset);

					if (master_socket == fdmax) {
						fdmax--;
					}
				}
			}
		}
	}

	return 0;
}
