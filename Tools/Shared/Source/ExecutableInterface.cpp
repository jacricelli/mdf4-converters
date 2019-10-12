#include "ExecutableInterface.h"

#include "Library.h"

#include <fstream>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/trivial.hpp>
#include <boost/range/iterator_range.hpp>

#include "VersionInformation.h"

using namespace std::placeholders;
namespace bfs = boost::filesystem;
namespace blo = boost::log;
namespace bpo = boost::program_options;

namespace tools::shared {

    ExecutableInterface::ExecutableInterface(std::unique_ptr<ConverterInterface> interface) : interface(std::move(interface)) {
        commonOptions = std::make_shared<CommonOptions>();
    }

    int ExecutableInterface::main(int argc, char** argv) {
        // Register progress callback.
        interface->registerProgressCallback([=](auto && ...args){return updateProgress(args...);});

        // Configure which arguments to parse. Start with basic and then custom.
        configureParser();
        interface->configureParser(commandlineOptions);
        interface->configureFileParser(configFileOptions);

        // Parse options. Again, start with basic and continue to derived. The commandline parser is allowed to take
        // ALL options, to ensure a default value is populated.
        boost::program_options::options_description allOptions("All allowed options");
        allOptions.add(commandlineOptions);
        allOptions.add(configFileOptions);

        auto parser = bpo::command_line_parser(argc, argv);
        parser.options(allOptions);
        parser.positional(commandlinePositionalOptions);
        parser.allow_unregistered();

        bpo::basic_parsed_options<char> parsedOptions(&commandlineOptions);

        ParseOptionStatus status = ParseOptionStatus::NoError;

        // Handle initial parsing of options from the command line.
        try {
            parsedOptions = parser.run();
            bpo::store(parsedOptions, optionResult);
            bpo::notify(optionResult);
        } catch (bpo::invalid_command_line_syntax &e) {
            switch(e.kind()) {
                case bpo::invalid_command_line_syntax::missing_parameter:
                    BOOST_LOG_TRIVIAL(error) << "Missing argument for option '" << e.get_option_name() << "'";
                    std::cout << "Missing argument for option '" << e.get_option_name() << "'";
                    break;
                default:
                    throw;
            }
            return -1;
        } catch (bpo::error &e) {
            BOOST_LOG_TRIVIAL(debug) << "Error occurred during initial input argument parsing of type " << typeid(e).name();
            throw;
        } catch (std::exception &e) {
            BOOST_LOG_TRIVIAL(fatal) << "Error occurred during initial input argument parsing: " << e.what();
            return -1;
        }

        // Continue on with the configuration file.
        bool noConfigFileFound = false;
        if(interface->usesConfigFile()) {
            std::string iniFileName = interface->programName + "_config.ini";

            auto configFilePath = bfs::weakly_canonical(iniFileName);

            if(bfs::exists(configFilePath)) {
                try {
                    auto configFile = std::ifstream(configFilePath.c_str());
                    auto configParserOptions = bpo::parse_config_file(configFile, configFileOptions, true);
                    bpo::store(configParserOptions, optionResult);
                    bpo::notify(optionResult);
                } catch (std::exception &e) {
                    BOOST_LOG_TRIVIAL(fatal) << "Error during parsing of configuration file: " << e.what();
                    return -1;
                }
            } else {
                // Delay logging until after the verbosity settings has been read.
                noConfigFileFound = true;
            }
        }

        // If no arguments are supplied, or an error occurred during parsing, display the help message.
        if(argc <= 1) {
            status |= ParseOptionStatus::DisplayHelp;
        }

        // Perform core parsing.
        try {
            status |= parseOptions(optionResult);
        } catch (std::exception &e) {
            BOOST_LOG_TRIVIAL(fatal) << "Error occurred during general input argument parsing: " << e.what();
            return -1;
        }

        // Perform specialization parsing.
        interface->setCommonOptions(commonOptions);
        try {
            status |= interface->parseOptions(optionResult);
        } catch (std::exception &e) {
            BOOST_LOG_TRIVIAL(fatal) << "Error occurred during specialized input argument parsing: " << e.what();
            return -1;
        }

        // Collect any unrecognized options.
        std::vector<std::string> unrecognizedOptions = bpo::collect_unrecognized(parsedOptions.options, bpo::exclude_positional);

        if(!unrecognizedOptions.empty()) {
            status |= ParseOptionStatus::UnrecognizedOption;
        }

        if(noConfigFileFound) {
            BOOST_LOG_TRIVIAL(info) << "No configuration file found, skipping.";
        }

        // Handle parsing result.
        if((status & ParseOptionStatus::UnrecognizedOption) == ParseOptionStatus::UnrecognizedOption) {
            displayUnrecognizedOptions(unrecognizedOptions);
            return 1;
        } else if((status & ParseOptionStatus::DisplayHelp) == ParseOptionStatus::DisplayHelp) {
            displayHelp();
            return 0;
        } else if((status & ParseOptionStatus::DisplayVersion) == ParseOptionStatus::DisplayVersion) {
            displayVersion();
            return 0;
        } else if((status & ParseOptionStatus::NoInputFiles) == ParseOptionStatus::NoInputFiles) {
            return 0;
        }

        // Create a mapping between all input files and their corresponding output files.
        // If the output directory is set, override the destination path, else place them in the same folder as the
        // input file.
        int returnCode = 0;

        for(auto & inputFilePath: inputFiles) {
            // Ensure the full path is used.
            if(!inputFilePath.is_absolute()) {
                inputFilePath = boost::filesystem::weakly_canonical(inputFilePath);
            }

            // Ensure that the current input file exists.
            if(!boost::filesystem::exists(inputFilePath)) {
                BOOST_LOG_TRIVIAL(error) << "File does not exist: " << inputFilePath;
                returnCode = 2;
                continue;
            }

            // Determine where to place the result.
            boost::filesystem::path outputFolder;
            if(optionResult.count("output-directory")) {
                // An output directory is specified, place everything here.
                outputFolder = boost::filesystem::path(optionResult["output-directory"].as<std::string>());

                if(!outputFolder.is_absolute()) {
                    boost::filesystem::weakly_canonical(outputFolder);
                }

                if(!boost::filesystem::exists(outputFolder)) {
                    BOOST_LOG_TRIVIAL(info) << "Output folder does not exists. Creating \"" << outputFolder << "\"";
                    try {
                        boost::filesystem::create_directories(outputFolder);
                    } catch (boost::filesystem::filesystem_error &e) {
                        BOOST_LOG_TRIVIAL(fatal) << "Could not create output folder \"" << outputFolder << "\". Logged error is:\n" << e.what();
                        return -1;
                    }
                }
            } else {
                // Use the same folder as the input file.
                outputFolder = inputFilePath.parent_path();
            }

            // Call the exporter for the conversion.
            if(!interface->convert(inputFilePath, outputFolder)) {
                BOOST_LOG_TRIVIAL(fatal) << "Error during conversion of \"" << inputFilePath << "\".";
                return -1;
            }
        }

        return returnCode;
    }

    void ExecutableInterface::configureParser() {
        // Add default options.
        commandlineOptions.add_options()
            ("help,h", bpo::bool_switch()->default_value(false), "Print this help message.")
            ("version,v", bpo::bool_switch()->default_value(false),"Print version information.")
            ("verbose", bpo::value<int>()->default_value(1), "Set verbosity of output (0-5).")
            ("input-directory,I", bpo::value<std::string>(), "Input directory to convert files from.")
            ("output-directory,O", bpo::value<std::string>(), "Output directory to place converted files into.")
            ("non-interactive", bpo::bool_switch()->default_value(false), "Run in non-interactive mode, with no progress output.")
            ("timezone,t", bpo::value<std::string>()->default_value("l"), "Display times in UTC (u), logger localtime (l, default) or PC local time (p).")
            ("input-files,i", bpo::value<std::vector<std::string>>(), "List of files to convert, ignored if input-directory is specified. All unknown arguments will be interpreted as input files.")
            ;

        // Capture all other options (Positional options) as input files.
        commandlinePositionalOptions.add("input-files", -1);
    }

    void ExecutableInterface::displayHelp() const {
        std::cout << "Usage:" << "\n";
        std::cout << interface->programName << " [-short-option value --long-option value] [-i] file_a [file_b ...]:\n";
        std::cout << "\n";
        std::cout << "Short options start with a single \"-\", while long options start with \"--\".\n";
        std::cout << "A value enclosed in \"[]\" signifies it is optional.\n";
        std::cout << "Some options only exists in the long form, while others exist in both forms.\n";
        std::cout << "Not all options require arguments (arg).\n";
        std::cout << "\n";
        std::cout << commandlineOptions << std::endl;
    }

    void ExecutableInterface::displayUnrecognizedOptions(std::vector<std::string> const& unrecognizedOptions) const {
        if(unrecognizedOptions.size() == 1) {
            std::cout << "Unrecognized option:" << "\n";
        } else {
            std::cout << "Unrecognized options:" << "\n";
        }

        for(auto const& option: unrecognizedOptions) {
            std::cout << option << "\n";
        }

        std::cout << "\n";
        displayHelp();
    }

    void ExecutableInterface::displayVersion() const {
        // Display version information on the base library, the shared base and the specialization.
        std::cout << "Version of " << interface->programName << ": " << interface->getVersion() << std::endl;
        std::cout << "Version of converter base: " << tools::shared::version << std::endl;
        std::cout << "Version of MDF library: " << mdf::version << std::endl;
    }

    ParseOptionStatus ExecutableInterface::parseOptions(boost::program_options::variables_map const& result) {
        // Handle request for help messages.
        if(result["help"].as<bool>()) {
            return ParseOptionStatus::DisplayHelp;
        }

        // Handle request for version information.
        if(result["version"].as<bool>()) {
            return ParseOptionStatus::DisplayVersion;
        }

        // Setup verbosity.
        switch(result["verbose"].as<int>()) {
            case 0:
                blo::core::get()->set_filter(blo::trivial::severity >= blo::trivial::fatal);
                break;
            case 1:
                blo::core::get()->set_filter(blo::trivial::severity >= blo::trivial::error);
                break;
            case 2:
                blo::core::get()->set_filter(blo::trivial::severity >= blo::trivial::warning);
                break;
            case 3:
                blo::core::get()->set_filter(blo::trivial::severity >= blo::trivial::info);
                break;
            case 4:
                blo::core::get()->set_filter(blo::trivial::severity >= blo::trivial::debug);
                break;
            case 5:
                blo::core::get()->set_filter(blo::trivial::severity >= blo::trivial::trace);
                break;
            default:
                // TODO: Send the value back as well.
                return ParseOptionStatus::UnrecognizedOption;
        }

        commonOptions->non_interactiveMode = result["non-interactive"].as<bool>();

        std::string timeZoneDisplay = "l";
        if(result.count("timezone")) {
            timeZoneDisplay = result["timezone"].as<std::string>();
        }

        char first = 0;
        if(timeZoneDisplay.length() != 0) {
            first = timeZoneDisplay[0];
        }

        switch(first) {
            case 'u':
                commonOptions->displayTimeFormat = UTC;
                break;
            case 'p':
                commonOptions->displayTimeFormat = PCLocalTime;
                break;
            case 'l':
                // Fallthrough for the default value.
            default:
                commonOptions->displayTimeFormat = LoggerLocalTime;
                break;
        }

        // Is an input directory specified? In that case, ignore any files passed to the program and instead populate
        // the files list from the directory.
        if(result.count("input-directory")) {
            // Ensure the location exists.
            boost::filesystem::path inputDirectory(result["input-directory"].as<std::string>());

            if(!inputDirectory.is_absolute()) {
                inputDirectory = boost::filesystem::weakly_canonical(inputDirectory);
            }

            // Ensure that the current input file exists.
            if(!boost::filesystem::exists(inputDirectory)) {
                // TODO: Log error.
                std::cout << inputDirectory << std::endl;
            } else if(!boost::filesystem::is_directory(inputDirectory)) {
                // TODO: Log error.
                std::cout << inputDirectory << std::endl;
            } else {
                for(auto& entry : boost::make_iterator_range(boost::filesystem::directory_iterator(inputDirectory), {})) {
                    if(boost::filesystem::is_regular_file(entry)) {
                        if(boost::filesystem::extension(entry) == ".mf4") {
                            inputFiles.push_back(entry);
                        }
                    }
                }
            }
        } else if(result.count("input-files")) {
            std::vector<std::string> files = result["input-files"].as<std::vector<std::string>>();
            for(std::string const& entry: files) {
                inputFiles.emplace_back(entry);
            }
        } else {
            // No input files.
            return ParseOptionStatus::NoInputFiles;
        }

        return ParseOptionStatus::NoError;
    }

    void ExecutableInterface::updateProgress(int current, int total) {
        // Do nothing if running in non-interactive mode.
        if(commonOptions->non_interactiveMode) {
            return;
        }

        // Determine the fraction to fill.
        const int width = 80;
        double fraction = static_cast<double>(current) / static_cast<double>(total);
        int fill = static_cast<int>(fraction * width);

        std::cout << "\r";

        for(int i = 0; i < fill - 1; i++) {
            std::cout << "=";
        }

        if(current != total) {
            std::cout << ">";
        }

        for(int i = fill; i < width; ++i) {
            std::cout << " ";
        }

        std::cout << " " << current << " / " << total;

        if(current == total) {
            std::cout << "\n";
        }

        std::cout.flush();
    }
}
