#pragma once

#include "config.h"

#include <string>
#include <unordered_set>
#include <vector>
#include <memory>

#include <Common/Logger.h>

#include <Poco/DOM/Document.h>
#include <Poco/DOM/DOMParser.h>
#include <Poco/DOM/DOMWriter.h>
#include <Poco/DOM/NodeList.h>
#include <Poco/DOM/NamedNodeMap.h>
#include <Poco/AutoPtr.h>
#include <Poco/DirectoryIterator.h>
#include <Poco/ConsoleChannel.h>
#include <Poco/Util/AbstractConfiguration.h>


namespace Poco { class Logger; }

namespace zkutil
{
    class ZooKeeperNodeCache;
    using EventPtr = std::shared_ptr<Poco::Event>;
}

namespace DB
{

using ConfigurationPtr = Poco::AutoPtr<Poco::Util::AbstractConfiguration>;
using XMLDocumentPtr = Poco::AutoPtr<Poco::XML::Document>;

class ConfigProcessor
{
public:
    using Substitutions = std::vector<std::pair<std::string, std::string>>;

    /// Set log_to_console to true if the logging subsystem is not initialized yet.
    explicit ConfigProcessor(
        const std::string & path,
        bool throw_on_bad_incl = false,
        bool log_to_console = false,
        const Substitutions & substitutions = Substitutions());

    /// Perform config includes and substitutions and return the resulting XML-document.
    ///
    /// Suppose path is "/path/file.xml"
    /// 1) Merge XML trees of /path/file.xml with XML trees of all files from /path/{conf,file}.d/*.{conf,xml}
    ///    * If an element has a "replace" attribute, replace the matching element with it.
    ///    * If an element has a "remove" attribute, remove the matching element.
    ///    * Else, recursively merge child elements.
    /// 2) Determine the includes file from the config: <include_from>/path2/metrika.xml</include_from>
    ///    If this path is not configured, use /etc/metrika.xml
    /// 3) Replace elements matching the "<foo incl="bar"/>" pattern with
    ///    "<foo>contents of the clickhouse/bar element in metrika.xml</foo>"
    /// 4) If zk_node_cache is non-NULL, replace elements matching the "<foo from_zk="/bar">" pattern with
    ///    "<foo>contents of the /bar ZooKeeper node</foo>".
    ///    If has_zk_includes is non-NULL and there are such elements, set has_zk_includes to true.
    XMLDocumentPtr processConfig(
        bool * has_zk_includes = nullptr,
        zkutil::ZooKeeperNodeCache * zk_node_cache = nullptr,
        const zkutil::EventPtr & zk_changed_event = nullptr,
        bool is_config_changed = true);

    static void processIncludes(
        XMLDocumentPtr & config,
        const Substitutions & substitutions,
        const std::string & include_from_path,
        bool throw_on_bad_incl,
        Poco::XML::DOMParser & dom_parser,
        const LoggerPtr & log,
        std::unordered_set<std::string> * contributing_zk_paths = {},
        std::vector<std::string> * contributing_files = {},
        zkutil::ZooKeeperNodeCache * zk_node_cache = {},
        const zkutil::EventPtr & zk_changed_event = {});

    static XMLDocumentPtr parseConfig(const std::string & config_path, Poco::XML::DOMParser & dom_parser);

    /// These configurations will be used if there is no configuration file.
    static void registerEmbeddedConfig(std::string name, std::string_view content);


    /// loadConfig* functions apply processConfig and create Poco::Util::XMLConfiguration.
    /// The resulting XML document is saved into a file with the name
    /// resulting from adding "-preprocessed" suffix to the path file name.
    /// E.g., config.xml -> config-preprocessed.xml

    struct LoadedConfig
    {
        ConfigurationPtr configuration;
        bool has_zk_includes;
        bool loaded_from_preprocessed;
        XMLDocumentPtr preprocessed_xml;
        std::string config_path;
    };

    /// If allow_zk_includes is true, expect that the configuration XML can contain from_zk nodes.
    /// If it is the case, set has_zk_includes to true and don't write config-preprocessed.xml,
    /// expecting that config would be reloaded with zookeeper later.
    LoadedConfig loadConfig(bool allow_zk_includes = false, bool is_config_changed = true);

    /// If fallback_to_preprocessed is true, then if KeeperException is thrown during config
    /// processing, load the configuration from the preprocessed file.
    LoadedConfig loadConfigWithZooKeeperIncludes(
        zkutil::ZooKeeperNodeCache & zk_node_cache,
        const zkutil::EventPtr & zk_changed_event,
        bool fallback_to_preprocessed = false,
        bool is_config_changed = true);

    /// Save preprocessed config to specified directory.
    /// If preprocessed_dir is empty - calculate from loaded_config.path + /preprocessed_configs/
    /// If skip_zk_encryption_keys == true, skip loading encryption keys with from_zk directive and decrypting config values,
    /// otherwise load/decrypt all types of keys
    void savePreprocessedConfig(LoadedConfig & loaded_config, std::string preprocessed_dir
#if USE_SSL
        , bool skip_zk_encryption_keys = false
#endif
    );

    /// Set path of main config.xml. It will be cut from all configs placed to preprocessed_configs/
    static void setConfigPath(const std::string & config_path);

    using Files = std::vector<std::string>;

    static Files getConfigMergeFiles(const std::string & config_path);

    /// Is the file named as result of config preprocessing, not as original files.
    static bool isPreprocessedFile(const std::string & config_path);

#if USE_SSL
    /// Encrypt text value
    static std::string encryptValue(const std::string & codec_name, const std::string & value);

    /// Decrypt value
    static std::string decryptValue(const std::string & codec_name, const std::string & value);
#endif

    static inline const auto SUBSTITUTION_ATTRS = {"incl", "from_zk", "from_env"};

private:
    const std::string path;
    std::string preprocessed_path;

    bool throw_on_bad_incl;

    LoggerPtr log;
    Poco::AutoPtr<Poco::Channel> channel_ptr;

    Substitutions substitutions;

    Poco::AutoPtr<Poco::XML::NamePool> name_pool;
    Poco::XML::DOMParser dom_parser;

    using NodePtr = Poco::AutoPtr<Poco::XML::Node>;

#if USE_SSL
    /// Decrypt elements in XML tree recursively starting with config_root
    static void decryptRecursive(Poco::XML::Node * config_root);
    /// Decrypt elements in config with specified encryption attributes and previously loaded encryption keys
    static void decryptEncryptedElements(LoadedConfig & loaded_config);

    /// Determine if there is a node starting inside config_root which has a descendant with a given attribute
    static bool hasNodeWithAttribute(Poco::XML::Node * config_root, const std::string & attribute_name);
    /// Determine if there is a node starting inside config_root with a given node_name which has a descendant with a given attribute
    static bool hasNodeWithNameAndChildNodeWithAttribute(Poco::XML::Node * config_root, const std::string & node_name, const std::string & attribute_name);
    /// Determine if there is a node in loaded_config with a given node_name which has a descendant with a given attribute
    static bool hasNodeWithNameAndChildNodeWithAttribute(LoadedConfig & loaded_config, const std::string & node_name, const std::string & attribute_name);
#endif

    void hideRecursive(Poco::XML::Node * config_root);
    XMLDocumentPtr hideElements(XMLDocumentPtr xml_tree);

    static void mergeRecursive(XMLDocumentPtr config, Poco::XML::Node * config_root, const Poco::XML::Node * with_root);

    /// If config root node name is not 'clickhouse' and merging config's root node names doesn't match, bypasses merging and returns false.
    /// For compatibility root node 'yandex' considered equal to 'clickhouse'.
    bool merge(XMLDocumentPtr config, XMLDocumentPtr with);

    static void doIncludesRecursive(
            XMLDocumentPtr config,
            XMLDocumentPtr include_from,
            const Substitutions & substitutions,
            bool throw_on_bad_incl,
            Poco::XML::DOMParser & dom_parser,
            const LoggerPtr & log,
            Poco::XML::Node * node,
            zkutil::ZooKeeperNodeCache * zk_node_cache,
            const zkutil::EventPtr & zk_changed_event,
            std::unordered_set<std::string> * contributing_zk_paths);
};

}
