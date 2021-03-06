#ifndef RBX_ENVIRONMENT_HPP
#define RBX_ENVIRONMENT_HPP

#include <string>
#include <stdexcept>

#include "vm.hpp"
#include "config_parser.hpp"
#include "configuration.hpp"

namespace rubinius {

  class ConfigParser;
  class QueryAgent;

  class Environment {
    // The thread used to trigger preemptive thread switching
    pthread_t preemption_thread_;

    int argc_;
    char** argv_;

  public:
    SharedState* shared;
    VM* state;
    QueryAgent* agent;

    ConfigParser  config_parser;
    Configuration config;

  public:
    Environment(int argc, char** argv);
    ~Environment();

    void setup_cpp_terminate();

    void load_config_argv(int argc, char** argv);
    void load_argv(int argc, char** argv);
    void load_kernel(std::string root);
    void load_directory(std::string dir);
    void load_platform_conf(std::string dir);
    void load_conf(std::string path);
    void load_string(std::string str);
    void run_file(std::string path);
    void run_from_filesystem(std::string root);
    void boot_vm();

    void enable_preemption();
    // Run in a seperate thread to provide preemptive thread
    // scheduling.
    void scheduler_loop();

    void halt();
    int exit_code();
    void start_signals();
    void start_agent(int port);
  };

}

#endif
