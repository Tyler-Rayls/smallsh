#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>

// Used for tracking the users use of Ctrl + Z which toggles the ability to run processes
// in the foreground or background. 
bool fgOnly = false;

/*
*   Struct for storing the different elements of the command line
*
*   command - the command to execute, e.g. exit or ls
*   args (optional) - arguments for the command
*   iRedirect (optional) - where to redirect the input from
*   oRedirect (optional) - where to redirect the output to
*   background (optional) - whether to run the process in the background or not
*/ 
struct commandStruct {
    char *command;
    char *args[514];
    char *iRedirect;
    char *oRedirect;
    bool background;
};

/*
*   Signal handler for the SIGTSTP signal. Toggles the ability to run processes in the background. 
*   Uses the fgOnly global variable to keep track of status of this feature. 
*/
void handle_SIGTSTP() {
    if (fgOnly == false) {
        // Turns off the ability to run processes in the background
        fgOnly = true;
        char *message = "\nEntering foreground-only mode (& is now ignored)\n";
        write(STDOUT_FILENO, message, 51);
        fflush(stdout);
    } else {
        // Turns on the ability to run processes in the background
        fgOnly = false;
        char *message = "\nExiting foreground-only mode\n";
        write(STDOUT_FILENO, message, 31);
        fflush(stdout);
    }
}

/*
*   Frees up the memory that was allocated on the heap for the command line struct. 
*   This is necessary to prevent accidental errors such as redirecting input/output
*   when it is not intended to places specified in previous commands. 
*/
void clearStruct(struct commandStruct *commandLine) {
    // Frees up the space for the command if there was one
    if (commandLine->command != NULL) {
        free(commandLine->command);
    }

    // Frees up all the arguments that were stored in the array
    for (int i = 0; i < 515; i++) {
        if (commandLine->args[i] != NULL) {
            free(commandLine->args[i]);
        } else {
            i = 515;
        }
    }

    // Frees up input redirection if it exists
    if (commandLine->iRedirect != NULL) {
        free(commandLine->iRedirect);
    }

    // Frees up the output redirection if it exists
    if (commandLine->oRedirect != NULL) {
        free(commandLine->oRedirect);
    }
}

/*
*   Checks to see if any background processes have finished executing.
*   Prints the pid and status of a background process that has finished. 
*/
void checkBackground(pid_t bgPids[]) {
    // Checks to see if a background process has finished and stores its status
    // and pid 
    int bgStatus;
    pid_t childPid = waitpid(-1, &bgStatus, WNOHANG);

    // Validates that waitpid returned a process id and not -1 because no process was found
    if (childPid > 0) {
        // Checks to see what the status of the process was
        if (WIFEXITED(bgStatus)) {
            // Prints a message if the process finished successfully
            printf("background pid %d is done: exit value %d\n", childPid, WEXITSTATUS(bgStatus));
            fflush(stdout);
        } else if (WIFSIGNALED(bgStatus)) {
            // Prints a message if a process was terminated by a signal
            printf("background pid %d is done: terminated by signal %d\n", childPid, WTERMSIG(bgStatus));
            fflush(stdout);
        }

        // Goes through the background process ids and removes the one that finished from the array
        for (int i = 0; i < 20; i++) {
            if (bgPids[i] == childPid) {
                bgPids[i] = 0;
            }
        }
    }
}

/*
*   Exits the shell. Kills all processes or jobs that the shell has started before 
*   it terminates itself. 
*/ 
void exitShell(struct commandStruct *commandLine, pid_t bgPids[]) {
    int childStatus;
    int i = 0;

    // Loops through the array of background processes and kills each process that is
    // running
    while (bgPids[i] > 0) {
        kill(bgPids[i], 9);
        waitpid(bgPids[i], &childStatus, WNOHANG);
        i++;
    }

    // Exits the program successfully
    exit(0);
}

/*
*   Changes the working directory of smallsh.
*
*   Changes to the optional first argument that is passed or to the HOME environment
*   variable. Supports absolute and relative paths. 
*/
void changeDirectory(char *path) {
    // Checks to see if the user specifed a path before changing to it or the 
    // HOME environment variable
    if (path == NULL) {
        chdir(getenv("HOME"));
    } else {
        chdir(path);
    }
}

/*
*   Forks a child process and uses execvp to replace it with a new program. The new program 
*   executes the command that the user entered. When the command ends the child process is 
*   terminated and returns to the parent to continue using the shell. 
*
*   Code for executing this mostly comes from an example in the exploration page: 
*       Process API - Executing a New Program
*/
void executeCommand(struct commandStruct *commandLine, pid_t bgPids[], char *statusText, int *statusCode, struct sigaction SIGINT_action, struct sigaction SIGTSTP_action) {
    int childStatus;
            
    // Forks a new process
    pid_t childPid = fork();

    int i = 0;
    int inputFD, outputFD, result;

    switch(childPid) {
        case -1:
            // Exits if there was an error with forking a child process
            perror("fork()");
            exit(1);
            break;
        case 0:
            // Executes only in the child process
            // Installs a handler so that the child process ignores SIGTSTP signals
            SIGTSTP_action.sa_handler = SIG_IGN;
            // Installs the SIGTSTP handler
            sigaction(SIGTSTP, &SIGTSTP_action, NULL);

            // Changes the process for handling the SIGINT signal for processes running in the foreground
            // Processes running in the foreground should respond to SIGINT with the default behavior
            if (commandLine->background == false) {
                SIGINT_action.sa_handler = SIG_DFL;
                sigaction(SIGINT, &SIGINT_action, NULL);
            }

            // Redirects the input if the program is to run in the background or input redirect has been specified
            if (commandLine->background == true || commandLine->iRedirect != NULL) {
                // Checks if a input redirect has been specified and sets it to that
                // If not, sets the input redirect to /dev/null for the background process
                if (commandLine->iRedirect != NULL) {
                    inputFD = open(commandLine->iRedirect, O_RDONLY);
                } else {
                    inputFD = open("/dev/null", O_RDONLY);
                }

                // Checks if there was an error opening the file
                if (inputFD == -1) {
                    printf("cannot open %s for input\n", commandLine->iRedirect);
                    fflush(stdout);
                }

                // Redirects stdin to the input file
                result = dup2(inputFD, 0);

                // Checks if there was an error redirecting the input
                if (result == -1) {
                    exit(1);
                }
            }

            // Redirects the input if the program is to run in the background or output redirect has been specified
            if (commandLine->background == true || commandLine->oRedirect != NULL) {
                // Checks if an output redirect has been specified and sets it to that
                // If not, sets the output redirect to /dev/null for the background process
                if (commandLine->oRedirect != NULL) {
                    outputFD = open(commandLine->oRedirect, O_WRONLY | O_CREAT | O_TRUNC, 0640);
                } else {
                    outputFD = open("/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0640);
                }

                // Checks if there was an error opening the file
                if (outputFD == -1) {
                    printf("cannot open %s for output\n", commandLine->oRedirect);
                    fflush(stdout);
                }

                // Redirects stdout to the output file
                result = dup2(outputFD, 1);

                // Checks if there was an error redirecting the output
                if (result == -1) {
                    exit(1);
                }
            }

            // Changes the program with the command and arguments that were entered
            execvp(commandLine->command, commandLine->args);

            // Only returns if there is an error
            perror(commandLine->command);
            exit(1);
            break;
        default:
            // Executes only in the parent process
            // Signals to close the input and output files once the execvp fork has finished
            // executing
            fcntl(inputFD, F_SETFD, FD_CLOEXEC);
            fcntl(outputFD, F_SETFD, FD_CLOEXEC);

            // Checks if the command should execute in the background or foreground
            // Only blocks if the command is executing in the foreground
            if (commandLine->background == true) {
                // Displays a message the process id is running in the background
                printf("background pid is %d\n", childPid);
                fflush(stdout);
                // Adds the process id to an array of pids running in the background
                for (i = 0; i < 20; i++) {
                    // Places the child pid in the first empty position
                    if (bgPids[i] == 0) {
                        bgPids[i] = childPid;
                        break;
                    }
                }
                // Gives control back to the shell while the process execute in the background
                childPid = waitpid(childPid, &childStatus, WNOHANG);
            } else {
                childPid = waitpid(childPid, &childStatus, 0);
                // Gets the status from the child process 
                if (WIFEXITED(childStatus)) {
                    // Stores a successful exit message and code
                    strcpy(statusText, "exit value");
                    *statusCode = WEXITSTATUS(childStatus);
                } else if (WIFSIGNALED(childStatus)) {
                    // Stores a terminated message and code
                    strcpy(statusText, "terminated by signal");
                    *statusCode = 2;
                    // Prints the message and code
                    printf("%s %d\n", statusText, 2);
                    fflush(stdout);
                }
            }
            break;
    }
}

/*
*   Parses the input and stores the elements of the command line in a struct. 
*   
*   Command Line Syntax:
*       command [arg1 arg2 ...] [< input_file] [> output_file] [&]
*
*   Elements in square brackets are optional. Input_file and output_file are files
*   to redirect input and/or output to. The optional & signals to execute the command
*   in the background. 
*/
struct commandStruct *parseInput(char *input, bool fgOnly) {
    // Creates a new commandStruct on the heap and assumes it will be a foreground process
    struct commandStruct *newCommand = malloc(sizeof(struct commandStruct));
    newCommand->background = false;

    // For use with strtok_r
    char *saveptr;

    // The first space delimited token is the command
    char *token = strtok_r(input, " \n", &saveptr);

    // Checks that there was a command before proceeding
    if (token == NULL) {
        return NULL;
    }

    // Allocates memory for the command string on the heap and points the struct 
    // attribute to it and the first value of the args attribute to it. 
    // First value of the args attribute is pointed to it for usage of execvp when
    // necessary. 
    newCommand->command = calloc(strlen(token) + 1, sizeof(char));
    newCommand->args[0] = calloc(strlen(token) + 1, sizeof(char));
    strcpy(newCommand->command, token);
    strcpy(newCommand->args[0], token);

    // Moves to the next token and initializes the index position of the arguments array
    token = strtok_r(NULL, " \n", &saveptr);
    int i = 1;

    // Executes through the input string and finds all of the arguments
    while (token != NULL && strcmp(token, "<") != 0 && strcmp(token, ">") != 0 && strcmp(token, "&") != 0) {
        // Allocates memory on the heap for the size of the current token and stores
        // the pointer for this location on the heap at the current index i of args. 
        newCommand->args[i] = calloc(strlen(token) + 1, sizeof(char));
        // Copies the token to the memory allocated on the heap, increments the
        // index position, and moves to the next token
        strcpy(newCommand->args[i], token);
        i++;
        token = strtok_r(NULL, " \n", &saveptr);
    }

    // Adds a null pointer to the end of the arguments array
    newCommand->args[i] = malloc(sizeof(char));
    newCommand->args[i] = NULL;

    do {
        if (token != NULL && strcmp(token, "<") == 0) {
            // Moves to the next token and allocates memory to place it as the 
            // input redirection 
            token = strtok_r(NULL, " \n", &saveptr);
            newCommand->iRedirect = calloc(strlen(token) + 1, sizeof(char));
            strcpy(newCommand->iRedirect, token);
        } else if (token != NULL && strcmp(token, ">") == 0) {
            // Moves to the next token and allocates memory to place it as the 
            // ouput redirection 
            token = strtok_r(NULL, " \n", &saveptr);
            newCommand->oRedirect = calloc(strlen(token) + 1, sizeof(char));
            strcpy(newCommand->oRedirect, token);
        } else if (token != NULL && strcmp(token, "&") == 0 && fgOnly == false) {
            // Sets the background boolean to true to signify that the process should
            // be ran in the background
            newCommand->background = true;
        }

        // Moves to the next token
        token = strtok_r(NULL, " \n", &saveptr);
    } while (token != NULL);

    return newCommand;
}

/*
*   Replaces all instances of the expansion variable, $$, with the process id of 
*   the smallsh program. Stores the result on the heap and returns a pointer to this
*   string. 
*
*   Algorithm found here:
*   https://www.geeksforgeeks.org/c-program-replace-word-text-another-given-word/
*/
char *expandVariable(char *input) {
    // Initalizes the expansion variable to look for in the input 
    char pidVariable[] = "$$";

    // Converts the process id of smallsh to a string and stores it in pidString
    // Help for getting the size of the process id from:
    // https://stackoverflow.com/questions/8090888/what-is-the-max-possible-length-of-a-pid-of-a-process-64-bit
    char tmpChar;
    char pidString[sizeof(snprintf(&tmpChar, 1, "%d", getpid())) + 1];
    sprintf(pidString, "%d", getpid());

    // Initializes variables used for finding and replacing the expansion variable
    // and returning the result
    char *result;
    int i, count = 0;
    int varLength = 2;
    int pidLength = strlen(pidString);

    // Counts the number of times $$ is found in the input
    // Iterates over the characters in input 1 at a time until it reaches the null terminating char
    for (i = 0; input[i] != '\0'; i++) {
        // Checks if $$ is found between the current position in the input and the end
        if (strstr(&input[i], pidVariable) == &input[i]) {
            // Increments the counter if the location of $$ is the current location and 
            // advances the pointer to the index after the location of $$
            count++;
            i += varLength - 1;
        }
    }

    // Creates a new string that is long enough to replace the occuranges of $$ with the process ID
    result = (char*)malloc(i + count * (pidLength - varLength) + 1);

    // Loops through characters in the 
    i = 0;
    while (*input) {
        // Checks if a $$ has been reached
        if (strstr(input, pidVariable) == input) {
            // Copies the process ID to the result 
            strcpy(&result[i], pidString);
            // Advances the pointer in the input string
            i += pidLength;
            input += varLength;
        } else {
            // Adds the characters following the $$ in the input to the result after 
            // the pid
            result[i++] = *input++;
        }
    }
    // Adds a null terminator to the result string and returns it
    result[i] = '\0';
    return result;
}

/*
*   Prompts the user to type in a command line and stores the input.
*/
char* getInput() {
    // Displays the command line prompt
    printf(": ");
    fflush(stdout);

    // Stores input from stdin up to 2048 characters or until a new line character
    // is reached at the location pointed to by input
    char *inputText = calloc(2049, sizeof(char));
    fgets(inputText, 2048, stdin);

    return inputText;
}

/*
*   smallsh is a small shell with features similar to the other shells such as bash. 
*
*   Contains built in methods for exiting the shell, changing the directory, and getting 
*   the status of the most recent process to terminate. Forks and execs other programs such 
*   as ls, pwd, kill, etc. 
*
*   User can also toggle the ability to run processes in the background or not using Ctrl + Z. 
*/
int main() {
    // Prints the introduction to the program
    printf("$ smallsh\n");
    fflush(stdout);

    // Initialize sigaction struct for handling the SIGTSTP signal to toggle foreground mode
    struct sigaction SIGTSTP_action = {0};
    SIGTSTP_action.sa_handler = handle_SIGTSTP;
    sigfillset(&SIGTSTP_action.sa_mask);
    SIGTSTP_action.sa_flags = 0;

    // Installs the SIGTSTP handler
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);

    // Initalize sigaction struct for handling the SIGINT signal to ignore it
    struct sigaction SIGINT_action = {0};
    SIGINT_action.sa_handler = SIG_IGN;
    sigfillset(&SIGINT_action.sa_mask);
    SIGINT_action.sa_flags = 0;

    // Installs the SIGINT handler
    sigaction(SIGINT, &SIGINT_action, NULL);

    // Initializes storage for the user input and a pointer to store the expanded input
    // with instances of "$$" replaced by the process id of the smallsh program
    char *input;
    char *expandedInput;

    // Initializes a pointer to a struct containing the elements of our expanded command line
    struct commandStruct *commandLine;

    // Initializes an array to keep track of background child process IDs
    pid_t bgPids[256] = {0};

    // Initalizes storage of the status text and code of the most recent foreground process to finish
    // Status text starts as "exit value" and the code starts as 0 before any process that are not built
    // in are ran
    char statusText[25];
    int statusCode = 0;
    strcpy(statusText, "exit value");

    // Loop executes while the running boolean is true.
    // running is changed to false when the command entered by the user is exit
    bool running = true;
    while (running == true) {
        // Gets the input from the user
        input = getInput();

        // Expands the variable $$ by replacing all instances of it in the input with 
        // the process id of this smallsh program
        expandedInput = expandVariable(input);

        // Parses the expanded input and stores the segments of it in a commandStruct
        // pointed to by commandLine
        commandLine = parseInput(expandedInput, fgOnly);

        // Checks that the command is not null, empty, or a comment and routes the 
        // command line to the appropriate function to execute the command
        if (commandLine != NULL && strcmp(commandLine->command, " ") != 0 && commandLine->command[0] != '#') {
            if (strcmp(commandLine->command, "exit") == 0) {
                // Kills all of the children processes and exits the shell
                exitShell(commandLine, bgPids);
            } else if (strcmp(commandLine->command, "cd") == 0) {
                // Changes the directory to the first argument that was passed in the command line
                changeDirectory(commandLine->args[1]);
            } else if (strcmp(commandLine->command, "status") == 0) {
                // Prints the status of the most recent process to finish executing
                printf("%s %d\n", statusText, statusCode);
                fflush(stdout);
            } else {
                // Handles all other commands entered
                executeCommand(commandLine, bgPids, statusText, &statusCode, SIGINT_action, SIGTSTP_action);
            }
        }

        // Checks to see if any background processes have finished before prompting for another input
        checkBackground(bgPids);

        // Frees up all the memory allocated on the heap 
        free(expandedInput);
        if (commandLine != NULL) {
            clearStruct(commandLine);
        }
    }

    return 0;
}