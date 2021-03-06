#ifndef __CONSOLE_HPP__
#define __CONSOLE_HPP__

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <tuple>
#include <iostream>
#include <map>

enum pid_dic { CHILD = 0 };
enum pip_dic { PIPEIN = 1, PIPEOUT = 0};
enum res_dic { DEFAULT, PIPE, FL};

struct Command
{
	int proc_id;
	std::vector<std::string> argv;
	int pipe_to;
	std::string filename;

	Command() : proc_id(-1), pipe_to(0), filename()
	{}
};

template <class Parser>
class Console : public Parser // Policy-based design class
{
private:
	std::string cmd_line;
	std::map<int, std::tuple<int, int>> pipe_lookup;
	int proc_counter;

	typedef std::vector<std::vector<std::string>> parse_tree;
	typedef std::vector<Command> command_vec;
	typedef Command command_t;

public:
	Console() : proc_counter(0)
	{
		setenv("PATH", "bin:.", true);
	}


	void replace_fd(int new_fd)
	{
		dup2(new_fd, 0);
		dup2(new_fd, 1);
		dup2(new_fd, 2);
	}


	void run()
	{
		std::cout << get_MOTD();

		while (get_command())
		{
			auto parsed_result = parse_cmd(cmd_line);
			auto commands = setup_cmd(parsed_result);

			if (execute_builtin_cmd(commands) == false)
				break;

			execute_cmd(commands);
		}
	}


	parse_tree parse_cmd(std::string &cmd_line)
	{
		parse_tree result;

		if (cmd_line.find("/") != std::string::npos)
		{
			std::cout << "Permission denied.\n";
		}
		else
		{
			std::vector<std::string> single_command;

			auto tokens = Parser::split(cmd_line);
			for (auto &token : tokens)
			{
				single_command.push_back(token);
				
				const bool is_pipe_symbol = token.find("|") != std::string::npos;
				if (is_pipe_symbol)
				{
					result.push_back(std::vector<std::string>());
					result.back().swap(single_command);
				}
			}

			if (single_command.size() != 0)
			{
				result.push_back(std::move(single_command));
			}
		}

		return result;
	}


	command_vec setup_cmd(parse_tree &parsed_cmd)
	{
		command_vec commands;

		for (auto &argv : parsed_cmd)
		{
			command_t cmd;

			if (argv.back().find("|") != std::string::npos)
			{
				int pipe_to = 1;
				if (argv.back().length() != 1)
				{
					pipe_to = std::atoi(argv.back().c_str() + 1);
				}

				cmd.pipe_to = pipe_to;
				argv.pop_back();
			}

			if (argv.size() > 2)
			{
				if (argv[argv.size() - 2] == ">")
				{
					cmd.filename = argv.back();
					argv.resize(argv.size() - 2);
				}
			}

			cmd.argv = argv;

			commands.push_back(std::move(cmd));
		}

		verify_cmd(commands);

		return commands;
	}


	void verify_cmd(command_vec &commands)
	{
		bool is_terminated = false;
		std::string path_all(getenv("PATH"));
		std::vector<std::string> paths;

		paths = Parser::split(path_all, ":");

		bool is_cmd_found;
		for (auto &cmd : commands)
		{
			is_cmd_found = false;

			// Test executable in each path
			for (auto &path : paths)
			{
				if (is_file_exist(cmd.argv[0], path))
				{
					cmd.proc_id = proc_counter++;

					is_cmd_found = true;
					break;
				}
			}

			if (is_cmd_found == false) break;
		}
	}


	bool execute_builtin_cmd(command_vec &commands)
	{
		for (auto it = commands.begin(); it != commands.end(); ++it)
		{
			auto &cmd  = it->argv;
			if (cmd[0] == "printenv")
			{
				if (cmd.size() == 2)
					std::cout << cmd[1] << "=" << getenv(cmd[1].c_str()) << "\n";

				commands.erase(it);
			}
			else if (cmd[0] == "setenv")
			{
				if (cmd.size() == 3)
					setenv(cmd[1].c_str(), cmd[2].c_str(), true);

				commands.erase(it);
			}
			else if (cmd[0] == "exit")
			{
				return false;
			}

			if (commands.size() < 1) break;
		}
		return true;
	}

	void execute_cmd(command_vec &commands)
	{
		for (auto &cmd : commands)
		{
			if (cmd.proc_id != -1)
			{
				execute(cmd);
			}
			else
			{
				std::cerr << "Unknown command: [" << cmd.argv[0] << "].\n";
				break;
			}
		}
	}


	inline std::vector<char *> c_style(std::vector<std::string> &vec_str)
	{
		std::vector<char *> vec_charp;
		for (auto &str : vec_str)
		{
			vec_charp.push_back((char *)str.c_str());
		}
		vec_charp.push_back(NULL);

		return vec_charp;
	}

	void execute(command_t &cmd)
	{
		const bool need_pipe = (cmd.pipe_to > 0);
		const bool need_file = (cmd.filename.length() > 0);

		if (need_pipe)
		{
			register_pipe(cmd.proc_id + cmd.pipe_to - 1);
		}

		pid_t child_pid = fork();
		if (child_pid < 0)
		{
			std::cerr << "fork child failed.\n";
			exit(1);
		}

		auto prev = pipe_lookup.find(cmd.proc_id - 1);

		if (child_pid == CHILD)
		{
			if (prev != pipe_lookup.end())
			{
				const int infd = std::get<PIPEOUT>(prev->second);
				dup2(infd, 0);
				close(std::get<PIPEIN>(prev->second));
			}

			if (need_pipe)
			{
				auto next = pipe_lookup.find(cmd.proc_id + cmd.pipe_to - 1);
				if (next != pipe_lookup.end())
				{
					const int outfd = std::get<PIPEIN>(next->second);
					dup2(outfd, 1);
					close(std::get<PIPEOUT>(next->second));

					if (need_file)
						close(std::get<PIPEIN>(next->second));
				}
			}

			if (need_file)
			{
				const int oflags = O_CREAT | O_WRONLY;
				const mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH;
				const int filefd = open(cmd.filename.c_str(), oflags, mode);
				if (filefd < 0)
				{
					std::cerr << "File open failed!\n";
					exit(EXIT_FAILURE);
				}

				dup2(filefd, 1);
			}

			auto argv = c_style(cmd.argv);
			execvp(argv[0], argv.data());
		}
		else
		{
			unregister_pipe(cmd.proc_id - 1);
			
			int status;
			if (wait(&status) < 0)
			{
				std::cerr << "Wait child failed.\n";
				exit(EXIT_FAILURE);
			}
		}
	}


	inline bool is_file_exist(std::string &filename, std::string &prefix)
	{
		std::string testname = prefix + "/" + filename;

		const bool is_exist = (access(testname.c_str(), F_OK) == 0);
		if (is_exist)
		{
			filename = testname;
		}

		return is_exist;
	}


	inline void register_pipe(int pipe_id)
	{
		auto it = pipe_lookup.find(pipe_id);
		if (it != pipe_lookup.end())
		{
			// This pipe is exist, so need not to register again!
			return;
		}

		int fd[2];
		if (pipe(fd) < 0)
		{
			std::cerr << "Pipe open failed!\n";
			exit(EXIT_FAILURE);
		}

		pipe_lookup[pipe_id] = std::make_tuple(fd[PIPEOUT], fd[PIPEIN]); // fd[0], fd[1]
	}


	inline void unregister_pipe(int pipe_id)
	{
		auto it = pipe_lookup.find(pipe_id);
		if (it != pipe_lookup.end())
		{
			close(std::get<PIPEIN>(it->second));
			close(std::get<PIPEOUT>(it->second));
			
			pipe_lookup.erase(it);
		}
	}


	inline bool get_command()
	{
		std::cout << "% ";
		return std::getline(std::cin, cmd_line);
	}


	std::string get_MOTD()
	{
		std::string motd;
		motd  = "****************************************\n";
		motd += "** Welcome to the information server. **\n";
		motd += "****************************************\n";
		return motd;
	}
};

#endif
