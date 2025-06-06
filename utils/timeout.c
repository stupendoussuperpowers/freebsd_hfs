#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

int main(int argc, char *argv[]) {

	if(argc < 2) {
		return 1;
	}
	
	pid_t child = fork();
	if (child == -1) {
		perror("fork");
		return 1;
	}

	if (child == 0) {
		printf("|| starting process ||\n");
		execvp(argv[1], &argv[1]);
		perror("execvp");
		exit(1);
	} else {
		sleep(5);
		kill(child, SIGABRT);

		printf("|| killed child ||\n");

		int status;
		waitpid(child, &status, 0);

		if (WIFSIGNALED(status)) {
			printf("Killed by signal: %d", status);
		} else {
			printf("Exited with code: %d", status);
		}
		
		return 0;
	}
}
