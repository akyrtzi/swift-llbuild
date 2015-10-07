//===-- BuildSystem.cpp ---------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "llbuild/BuildSystem/BuildSystem.h"
#include "llbuild/BuildSystem/BuildSystemCommandInterface.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"

#include "llbuild/Basic/FileInfo.h"
#include "llbuild/Basic/Hashing.h"
#include "llbuild/Core/BuildDB.h"
#include "llbuild/Core/BuildEngine.h"
#include "llbuild/Core/MakefileDepsParser.h"
#include "llbuild/BuildSystem/BuildExecutionQueue.h"
#include "llbuild/BuildSystem/BuildFile.h"
#include "llbuild/BuildSystem/BuildKey.h"
#include "llbuild/BuildSystem/BuildValue.h"

#include <memory>

using namespace llbuild;
using namespace llbuild::basic;
using namespace llbuild::core;
using namespace llbuild::buildsystem;

BuildExecutionQueue::~BuildExecutionQueue() {}

BuildSystemDelegate::~BuildSystemDelegate() {}

BuildSystemCommandInterface::~BuildSystemCommandInterface() {}

#pragma mark - BuildSystem implementation

namespace {

class BuildNode;
class BuildSystemImpl;

/// The delegate used to load the build file for use by a build system.
class BuildSystemFileDelegate : public BuildFileDelegate {
  BuildSystemImpl& system;
  
public:
  BuildSystemFileDelegate(BuildSystemImpl& system)
      : BuildFileDelegate(), system(system) {}

  BuildSystemDelegate& getSystemDelegate();

  /// @name Delegate Implementation
  /// @{

  virtual void setFileContentsBeingParsed(llvm::StringRef buffer) override;
  
  virtual void error(const std::string& filename,
                     const Token& at,
                     const std::string& message) override;

  virtual bool configureClient(const std::string& name,
                               uint32_t version,
                               const property_list_type& properties) override;

  virtual std::unique_ptr<Tool> lookupTool(const std::string& name) override;

  virtual void loadedTarget(const std::string& name,
                            const Target& target) override;

  virtual void loadedCommand(const std::string& name,
                             const Command& target) override;

  virtual std::unique_ptr<Node> lookupNode(const std::string& name,
                                           bool isImplicit=false) override;

  /// @}
};

/// The delegate used to build a loaded build file.
class BuildSystemEngineDelegate : public BuildEngineDelegate {
  BuildSystemImpl& system;

  // FIXME: This is an inefficent map, the string is duplicated.
  std::unordered_map<std::string, std::unique_ptr<BuildNode>> dynamicNodes;
  
  BuildFile& getBuildFile();

  virtual Rule lookupRule(const KeyType& keyData) override;
  virtual void cycleDetected(const std::vector<Rule*>& items) override;

public:
  BuildSystemEngineDelegate(BuildSystemImpl& system) : system(system) {}

  BuildSystemImpl& getBuildSystem() {
    return system;
  }
};

class BuildSystemImpl : public BuildSystemCommandInterface {
  BuildSystem& buildSystem;

  /// The delegate the BuildSystem was configured with.
  BuildSystemDelegate& delegate;

  /// The name of the main input file.
  std::string mainFilename;

  /// The delegate used for the loading the build file.
  BuildSystemFileDelegate fileDelegate;

  /// The build file the system is building.
  BuildFile buildFile;

  /// The delegate used for building the file contents.
  BuildSystemEngineDelegate engineDelegate;

  /// The build engine.
  BuildEngine buildEngine;

  /// The execution queue.
  std::unique_ptr<BuildExecutionQueue> executionQueue;

  /// @name BuildSystemCommandInterface Implementation
  /// @{

  virtual BuildEngine& getBuildEngine() override {
    return buildEngine;
  }
  
  virtual BuildExecutionQueue& getExecutionQueue() override {
    return *executionQueue;
  }

  virtual void taskNeedsInput(core::Task* task, const BuildKey& key,
                              uintptr_t inputID) override {
    return buildEngine.taskNeedsInput(task, key.toData(), inputID);
  }

  virtual void taskMustFollow(core::Task* task, const BuildKey& key) override {
    return buildEngine.taskMustFollow(task, key.toData());
  }

  virtual void taskDiscoveredDependency(core::Task* task,
                                        const BuildKey& key) override {
    return buildEngine.taskDiscoveredDependency(task, key.toData());
  }

  virtual void taskIsComplete(core::Task* task, const BuildValue& value,
                              bool forceChange) override {
    return buildEngine.taskIsComplete(task, value.toData(), forceChange);
  }

  virtual void addJob(QueueJob&& job) override {
    executionQueue->addJob(std::move(job));
  }

  /// @}

public:
  BuildSystemImpl(class BuildSystem& buildSystem,
                  BuildSystemDelegate& delegate,
                  const std::string& mainFilename)
      : buildSystem(buildSystem), delegate(delegate),
        mainFilename(mainFilename),
        fileDelegate(*this), buildFile(mainFilename, fileDelegate),
        engineDelegate(*this), buildEngine(engineDelegate),
        executionQueue(delegate.createExecutionQueue()) {}

  BuildSystem& getBuildSystem() {
    return buildSystem;
  }

  BuildSystemDelegate& getDelegate() {
    return delegate;
  }

  const std::string& getMainFilename() {
    return mainFilename;
  }

  BuildSystemCommandInterface& getCommandInterface() {
    return *this;
  }

  BuildFile& getBuildFile() {
    return buildFile;
  }

  void error(const std::string& filename, const std::string& message) {
    getDelegate().error(filename, {}, message);
  }

  void error(const std::string& filename, const BuildSystemDelegate::Token& at,
             const std::string& message) {
    getDelegate().error(filename, at, message);
  }

  std::unique_ptr<BuildNode> lookupNode(const std::string& name,
                                        bool isImplicit);

  /// @name Client API
  /// @{

  bool attachDB(const std::string& filename, std::string* error_out) {
    // FIXME: How do we pass the client schema version here, if we haven't
    // loaded the file yet.
    std::unique_ptr<core::BuildDB> db(
        core::createSQLiteBuildDB(filename, delegate.getVersion(),
                                  error_out));
    if (!db)
      return false;

    buildEngine.attachDB(std::move(db));
    return true;
  }

  bool enableTracing(const std::string& filename, std::string* error_out) {
    return buildEngine.enableTracing(filename, error_out);
  }

  bool build(const std::string& target);

  /// @}
};

#pragma mark - BuildSystem engine integration

#pragma mark - BuildNode implementation

// FIXME: Figure out how this is going to be organized.
class BuildNode : public Node {
  // FIXME: This is just needed for diagnostics during configuration, we should
  // make that some kind of context argument instead of storing it in every
  // node.
  BuildSystemImpl& system;
  
  /// Whether or not this node is "virtual" (i.e., not a filesystem path).
  bool virtualNode;

public:
  explicit BuildNode(BuildSystemImpl& system, const std::string& name,
                     bool isVirtual)
      : Node(name), system(system), virtualNode(isVirtual) {}

  bool isVirtual() const { return virtualNode; }

  virtual bool configureAttribute(const std::string& name,
                                  const std::string& value) override {
    if (name == "is-virtual") {
      if (value == "true") {
        virtualNode = true;
      } else if (value == "false") {
        virtualNode = false;
      } else {
        system.error(system.getMainFilename(),
                     "invalid value: '" + value +
                     "' for attribute '" + name + "'");
        return false;
      }
      return true;
    }
    
    // We don't support any other custom attributes.
    system.error(system.getMainFilename(),
                 "unexpected attribute: '" + name + "'");
    return false;
  }

  FileInfo getFileInfo() const {
    assert(!isVirtual());
    return FileInfo::getInfoForPath(getName());
  }
};

#pragma mark - Task implementations

static BuildSystemImpl& getBuildSystem(BuildEngine& engine) {
  return static_cast<BuildSystemEngineDelegate*>(
      engine.getDelegate())->getBuildSystem();
}
  
/// This is the task used to "build" a target, it translates between the request
/// for building a target key and the requests for all of its nodes.
class TargetTask : public Task {
  Target& target;
  
  // Build specific data.
  //
  // FIXME: We should probably factor this out somewhere else, so we can enforce
  // it is never used when initialized incorrectly.

  /// If true, the command had a missing input (this implies ShouldSkip is
  /// true).
  bool hasMissingInput = false;

  virtual void start(BuildEngine& engine) override {
    // Request all of the necessary system tasks.
    unsigned id = 0;
    for (auto it = target.getNodes().begin(),
           ie = target.getNodes().end(); it != ie; ++it, ++id) {
      engine.taskNeedsInput(this, BuildKey::makeNode(*it).toData(), id);
    }
  }

  virtual void providePriorValue(BuildEngine&,
                                 const ValueType& value) override {
    // Do nothing.
  }

  virtual void provideValue(BuildEngine&, uintptr_t inputID,
                            const ValueType& valueData) override {
    // Do nothing.
    auto value = BuildValue::fromData(valueData);

    if (value.isMissingInput()) {
      hasMissingInput = true;

      // FIXME: Design the logging and status output APIs.
      fprintf(stderr, "error: missing input '%s' and no rule to build it\n",
              target.getNodes()[inputID]->getName().c_str());
    }
  }

  virtual void inputsAvailable(BuildEngine& engine) override {
    if (hasMissingInput) {
      // FIXME: Design the logging and status output APIs.
      fprintf(stderr, "error: cannot build target '%s' due to missing input\n",
              target.getName().c_str());

      // Report the command failure.
      getBuildSystem(engine).getDelegate().hadCommandFailure();
    }
    
    // Complete the task immediately.
    engine.taskIsComplete(this, BuildValue::makeTarget().toData());
  }

public:
  TargetTask(Target& target) : target(target) {}

  static bool isResultValid(Target& node, const BuildValue& value) {
    // Always treat target tasks as invalid.
    return false;
  }
};

/// This is the task to "build" a node which represents pure raw input to the
/// system.
class InputNodeTask : public Task {
  BuildNode& node;

  virtual void start(BuildEngine& engine) override {
    assert(node.getProducers().empty());
  }

  virtual void providePriorValue(BuildEngine&,
                                 const ValueType& value) override {
  }

  virtual void provideValue(BuildEngine&, uintptr_t inputID,
                            const ValueType& value) override {
  }

  virtual void inputsAvailable(BuildEngine& engine) override {
    // Handle virtual nodes.
    if (node.isVirtual()) {
      engine.taskIsComplete(
          this, BuildValue::makeVirtualInput().toData());
      return;
    }
    
    // Get the information on the file.
    //
    // FIXME: This needs to delegate, since we want to have a notion of
    // different node types.
    auto info = node.getFileInfo();
    if (info.isMissing()) {
      engine.taskIsComplete(this, BuildValue::makeMissingInput().toData());
      return;
    }

    engine.taskIsComplete(
        this, BuildValue::makeExistingInput(info).toData());
  }

public:
  InputNodeTask(BuildNode& node) : node(node) {}

  static bool isResultValid(const BuildNode& node, const BuildValue& value) {
    // Virtual input nodes are always valid unless the value type is wrong.
    if (node.isVirtual())
      return value.isVirtualInput();
    
    // If the previous value wasn't for an existing input, always recompute.
    if (!value.isExistingInput())
      return false;

    // Otherwise, the result is valid if the path exists and the file
    // information remains the same.
    //
    // FIXME: This is inefficient, we will end up doing the stat twice, once
    // when we check the value for up to dateness, and once when we "build" the
    // output.
    //
    // We can solve this by caching ourselves but I wonder if it is something
    // the engine should support more naturally.
    auto info = node.getFileInfo();
    if (info.isMissing())
      return false;

    return value.getOutputInfo() == info;
  }
};


/// This is the task to "build" a node which is the product of some command.
///
/// It is responsible for selecting the appropriate producer command to run to
/// produce the node, and for synchronizing any external state the node depends
/// on.
class ProducedNodeTask : public Task {
  Node& node;
  BuildValue nodeResult;
  Command* producingCommand = nullptr;
  
  virtual void start(BuildEngine& engine) override {
    // Request the producer command.
    if (node.getProducers().size() == 1) {
      producingCommand = node.getProducers()[0];
      engine.taskNeedsInput(this, BuildKey::makeCommand(
                                producingCommand->getName()).toData(),
                            /*InputID=*/0);
      return;
    }

    // FIXME: Delegate to the client to select the appropriate producer if
    // there are more than one.
    assert(0 && "FIXME: not implemented (support for non-unary producers");
    abort();
  }

  virtual void providePriorValue(BuildEngine&,
                                 const ValueType& value) override {
  }

  virtual void provideValue(BuildEngine&, uintptr_t inputID,
                            const ValueType& valueData) override {
    auto value = BuildValue::fromData(valueData);

    // Extract the node result from the command.
    assert(producingCommand);
    nodeResult = std::move(producingCommand->getResultForOutput(&node, value));
  }

  virtual void inputsAvailable(BuildEngine& engine) override {
    assert(!nodeResult.isInvalid());
    
    // Complete the task immediately.
    engine.taskIsComplete(this, nodeResult.toData());
  }

public:
  ProducedNodeTask(Node& node)
      : node(node), nodeResult(BuildValue::makeInvalid()) {}
  
  static bool isResultValid(Node& node, const BuildValue& value) {
    // The produced node result itself doesn't need any synchronization.
    return true;
  }
};

/// This is the task to actually execute a command.
class CommandTask : public Task {
  Command& command;

  virtual void start(BuildEngine& engine) override {
    command.start(getBuildSystem(engine).getCommandInterface(), this);
  }

  virtual void providePriorValue(BuildEngine& engine,
                                 const ValueType& valueData) override {
    BuildValue value = BuildValue::fromData(valueData);
    command.providePriorValue(
        getBuildSystem(engine).getCommandInterface(), this, value);
  }

  virtual void provideValue(BuildEngine& engine, uintptr_t inputID,
                            const ValueType& valueData) override {
    command.provideValue(
        getBuildSystem(engine).getCommandInterface(), this, inputID,
        BuildValue::fromData(valueData));
  }

  virtual void inputsAvailable(BuildEngine& engine) override {
    command.inputsAvailable(getBuildSystem(engine).getCommandInterface(), this);
  }

public:
  CommandTask(Command& command) : command(command) {}

  static bool isResultValid(Command& command, const BuildValue& value) {
    // Delegate to the command for further checking.
    return command.isResultValid(value);
  }
};

#pragma mark - BuildSystemEngineDelegate implementation

BuildFile& BuildSystemEngineDelegate::getBuildFile() {
  return system.getBuildFile();
}

Rule BuildSystemEngineDelegate::lookupRule(const KeyType& keyData) {
  // Decode the key.
  auto key = BuildKey::fromData(keyData);

  switch (key.getKind()) {
  default:
    assert(0 && "invalid key");
    abort();

  case BuildKey::Kind::Command: {
    // Find the comand.
    auto it = getBuildFile().getCommands().find(key.getCommandName());
    if (it == getBuildFile().getCommands().end()) {
      assert(0 && "unexpected request for missing command");
      abort();
    }

    // Create the rule for the command.
    Command* command = it->second.get();
    return Rule{
      keyData,
      /*Action=*/ [command](BuildEngine& engine) -> Task* {
        return engine.registerTask(new CommandTask(*command));
      },
      /*IsValid=*/ [command](const Rule& rule, const ValueType& value) -> bool {
        return CommandTask::isResultValid(
            *command, BuildValue::fromData(value));
      }
    };
  }

  case BuildKey::Kind::Node: {
    // Find the node.
    auto it = getBuildFile().getNodes().find(key.getNodeName());
    BuildNode* node;
    if (it != getBuildFile().getNodes().end()) {
      node = static_cast<BuildNode*>(it->second.get());
    } else {
      auto it = dynamicNodes.find(key.getNodeName());
      if (it != dynamicNodes.end()) {
        node = it->second.get();
      } else {
        // Create nodes on the fly for any unknown ones.
        auto nodeOwner = system.lookupNode(
            key.getNodeName(), /*isImplicit=*/true);
        node = nodeOwner.get();
        dynamicNodes[key.getNodeName()] = std::move(nodeOwner);
      }
    }

    // Create the rule used to construct this node.
    //
    // We could bypass this level and directly return the rule to run the
    // command, which would reduce the number of tasks in the system. For now we
    // do the uniform thing, but do differentiate between input and command
    // nodes.

    // Create an input node if there are no producers.
    if (node->getProducers().empty()) {
      return Rule{
        keyData,
        /*Action=*/ [node](BuildEngine& engine) -> Task* {
          return engine.registerTask(new InputNodeTask(*node));
        },
        /*IsValid=*/ [node](const Rule& rule, const ValueType& value) -> bool {
          return InputNodeTask::isResultValid(
              *node, BuildValue::fromData(value));
        }
      };
    }

    // Otherwise, create a task for a produced node.
    return Rule{
      keyData,
      /*Action=*/ [node](BuildEngine& engine) -> Task* {
        return engine.registerTask(new ProducedNodeTask(*node));
      },
      /*IsValid=*/ [node](const Rule& rule, const ValueType& value) -> bool {
        return ProducedNodeTask::isResultValid(
            *node, BuildValue::fromData(value));
      }
    };
  }

  case BuildKey::Kind::Target: {
    // Find the target.
    auto it = getBuildFile().getTargets().find(key.getTargetName());
    if (it == getBuildFile().getTargets().end()) {
      // FIXME: Invalid target name, produce an error.
      assert(0 && "FIXME: invalid target");
      abort();
    }

    // Create the rule to construct this target.
    Target* target = it->second.get();
    return Rule{
      keyData,
        /*Action=*/ [target](BuildEngine& engine) -> Task* {
        return engine.registerTask(new TargetTask(*target));
      },
      /*IsValid=*/ [target](const Rule& rule, const ValueType& value) -> bool {
        return TargetTask::isResultValid(*target, BuildValue::fromData(value));
      }
    };
  }
  }
}

void BuildSystemEngineDelegate::cycleDetected(const std::vector<Rule*>& items) {
  system.error(system.getMainFilename(), "cycle detected while building");
}

#pragma mark - BuildSystemImpl implementation

std::unique_ptr<BuildNode>
BuildSystemImpl::lookupNode(const std::string& name, bool isImplicit) {
  bool isVirtual = !name.empty() && name[0] == '<' && name.back() == '>';
  return std::make_unique<BuildNode>(*this, name, isVirtual);
}

bool BuildSystemImpl::build(const std::string& target) {
  // Load the build file.
  //
  // FIXME: Eventually, we may want to support something fancier where we load
  // the build file in the background so we can immediately start building
  // things as they show up.
  //
  // FIXME: We need to load this only once.
  if (!getBuildFile().load()) {
    error(getMainFilename(), "unable to load build file");
    return false;
  }    

  // Build the target.
  getBuildEngine().build(BuildKey::makeTarget(target).toData());

  return true;
}

#pragma mark - ExternalCommand implementation

/// This is a base class for defining commands which are run externally to the
/// build system and interact using files. It defines common base behaviors
/// which make sense for all such tools.
class ExternalCommand : public Command {
  BuildSystemImpl& system;
  std::vector<BuildNode*> inputs;
  std::vector<BuildNode*> outputs;
  std::string description;

  // Build specific data.
  //
  // FIXME: We should probably factor this out somewhere else, so we can enforce
  // it is never used when initialized incorrectly.

  /// If true, the command should be skipped (because of an error in an input).
  bool shouldSkip = false;

  /// If true, the command had a missing input (this implies ShouldSkip is
  /// true).
  bool hasMissingInput = false;

protected:
  virtual uint64_t getSignature() {
    uint64_t result = 0;
    for (const auto* input: inputs) {
      result ^= basic::hashString(input->getName());
    }
    for (const auto* output: outputs) {
      result ^= basic::hashString(output->getName());
    }
    return result;
  }

  const std::vector<BuildNode*>& getInputs() { return inputs; }
  
  const std::vector<BuildNode*>& getOutputs() { return outputs; }
  
  const std::string& getDescription() { return description; }
  
public:
  ExternalCommand(BuildSystemImpl& system, const std::string& name)
      : Command(name), system(system) {}

  virtual void configureDescription(const std::string& value) override {
    description = value;
  }
  
  virtual void configureInputs(const std::vector<Node*>& value) override {
    inputs.reserve(value.size());
    for (auto* node: value) {
      inputs.emplace_back(static_cast<BuildNode*>(node));
    }
  }

  virtual void configureOutputs(const std::vector<Node*>& value) override {
    outputs.reserve(value.size());
    for (auto* node: value) {
      outputs.emplace_back(static_cast<BuildNode*>(node));
    }
  }

  virtual bool configureAttribute(const std::string& name,
                                  const std::string& value) override {
    system.error(system.getMainFilename(),
                 "unexpected attribute: '" + name + "'");
    return false;
  }

  virtual BuildValue getResultForOutput(Node* node,
                                        const BuildValue& value) override {
    // If the value was a failed or skipped command, propagate the failure.
    if (value.isFailedCommand() || value.isSkippedCommand())
      return BuildValue::makeFailedInput();

    // Otherwise, we should have a successful command -- return the actual
    // result for the output.
    assert(value.isSuccessfulCommand());

    // If the node is virtual, the output is always a virtual input value.
    if (static_cast<BuildNode*>(node)->isVirtual()) {
      return BuildValue::makeVirtualInput();
    }
    
    // Find the index of the output node.
    //
    // FIXME: This is O(N). We don't expect N to be large in practice, but it
    // could be.
    auto it = std::find(outputs.begin(), outputs.end(), node);
    assert(it != outputs.end());
    
    auto idx = it - outputs.begin();
    assert(idx < value.getNumOutputs());

    auto& info = value.getNthOutputInfo(idx);
    if (info.isMissing())
      return BuildValue::makeMissingInput();
    
    return BuildValue::makeExistingInput(info);
  }
  
  virtual bool isResultValid(const BuildValue& value) override {
    // If the prior value wasn't for a successful command, recompute.
    if (!value.isSuccessfulCommand())
      return false;
    
    // If the command's signature has changed since it was built, rebuild.
    if (value.getCommandSignature() != getSignature())
      return false;

    // Check the timestamps on each of the outputs.
    for (unsigned i = 0, e = outputs.size(); i != e; ++i) {
      auto* node = outputs[i];

      // Ignore virtual outputs.
      if (node->isVirtual())
        continue;
      
      // Always rebuild if the output is missing.
      auto info = node->getFileInfo();
      if (info.isMissing())
        return false;

      // Otherwise, the result is valid if the file information has not changed.
      if (value.getNthOutputInfo(i) != info)
        return false;
    }

    // Otherwise, the result is ok.
    return true;
  }

  virtual void start(BuildSystemCommandInterface& system, Task* task) override {
    // Initialize the build state.
    shouldSkip = false;
    hasMissingInput = false;

    // Request all of the inputs.
    unsigned id = 0;
    for (auto it = inputs.begin(), ie = inputs.end(); it != ie; ++it, ++id) {
      system.taskNeedsInput(task, BuildKey::makeNode(*it), id);
    }
  }

  virtual void providePriorValue(BuildSystemCommandInterface&, Task*,
                                 const BuildValue&) override {
  }

  virtual void provideValue(BuildSystemCommandInterface&, Task*,
                            uintptr_t inputID,
                            const BuildValue& value) override {
    // Process the input value to see if we should skip this command.

    // All direct inputs should be individual node values.
    assert(!value.hasMultipleOutputs());
    assert(value.isExistingInput() || value.isMissingInput() ||
           value.isFailedInput() || value.isVirtualInput());

    // If the value is not an existing or virtual input, then we shouldn't run
    // this command.
    if (!value.isExistingInput() && !value.isVirtualInput()) {
      shouldSkip = true;
      if (value.isMissingInput()) {
        hasMissingInput = true;

        // FIXME: Design the logging and status output APIs.
        fprintf(stderr, "error: missing input '%s' and no rule to build it\n",
                inputs[inputID]->getName().c_str());
      }
    }
  }

  virtual void inputsAvailable(BuildSystemCommandInterface& bsci,
                               Task* task) override {
    // If the build should cancel, do nothing.
    if (system.getDelegate().isCancelled()) {
      bsci.taskIsComplete(task, BuildValue::makeSkippedCommand());
      return;
    }
    
    // If this command should be skipped, do nothing.
    if (shouldSkip) {
      // If this command had a failed input, treat it as having failed.
      if (hasMissingInput) {
        // FIXME: Design the logging and status output APIs.
        fprintf(stderr, "error: cannot build '%s' due to missing input\n",
                outputs[0]->getName().c_str());

        // Report the command failure.
        system.getDelegate().hadCommandFailure();
      }

      bsci.taskIsComplete(task, BuildValue::makeSkippedCommand());
      return;
    }
    assert(!hasMissingInput);
    
    // Suppress static analyzer false positive on generalized lambda capture
    // (rdar://problem/22165130).
#ifndef __clang_analyzer__
    auto fn = [this, &bsci=bsci, task](QueueJobContext* context) {
      // Execute the command.
      if (!executeExternalCommand(bsci, task, context)) {
        // If the command failed, the result is failure.
        bsci.taskIsComplete(task, BuildValue::makeFailedCommand());
        system.getDelegate().hadCommandFailure();
        return;
      }

      // Capture the file information for each of the output nodes.
      //
      // FIXME: We need to delegate to the node here.
      llvm::SmallVector<FileInfo, 8> outputInfos;
      for (auto* node: outputs) {
        if (node->isVirtual()) {
          outputInfos.push_back(FileInfo{});
        } else {
          outputInfos.push_back(node->getFileInfo());
        }
      }
      
      // Otherwise, complete with a successful result.
      bsci.taskIsComplete(
          task, BuildValue::makeSuccessfulCommand(outputInfos, getSignature()));
    };
    bsci.addJob({ this, std::move(fn) });
#endif
  }

  /// Extension point for subclasses, to actually execute the command.
  virtual bool executeExternalCommand(BuildSystemCommandInterface& bsci,
                                      Task* task, QueueJobContext* context) = 0;
};

#pragma mark - PhonyTool implementation

class PhonyCommand : public ExternalCommand {
public:
  using ExternalCommand::ExternalCommand;

  virtual bool executeExternalCommand(BuildSystemCommandInterface& bsci,
                                      Task* task,
                                      QueueJobContext* context) override {
    // Nothing needs to be done for phony commands.
    return true;
  }
};

class PhonyTool : public Tool {
  BuildSystemImpl& system;

public:
  PhonyTool(BuildSystemImpl& system, const std::string& name)
      : Tool(name), system(system) {}

  virtual bool configureAttribute(const std::string& name,
                                  const std::string& value) override {
    // No supported configuration attributes.
    system.error(system.getMainFilename(),
                 "unexpected attribute: '" + name + "'");
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(
      const std::string& name) override {
    return std::make_unique<PhonyCommand>(system, name);
  }
};

#pragma mark - ShellTool implementation

class ShellCommand : public ExternalCommand {
  std::string args;

  virtual uint64_t getSignature() override {
    uint64_t result = ExternalCommand::getSignature();
    result ^= basic::hashString(args);
    return result;
  }
  
public:
  using ExternalCommand::ExternalCommand;
  
  virtual bool configureAttribute(const std::string& name,
                                  const std::string& value) override {
    if (name == "args") {
      args = value;
    } else {
      return ExternalCommand::configureAttribute(name, value);
    }

    return true;
  }

  virtual bool executeExternalCommand(BuildSystemCommandInterface& bsci,
                                      Task* task,
                                      QueueJobContext* context) override {
    // Log the command.
    //
    // FIXME: Design the logging and status output APIs.
    if (getDescription().empty()) {
      fprintf(stdout, "%s\n", args.c_str());
    } else {
      fprintf(stdout, "%s\n", getDescription().c_str());
    }
    fflush(stdout);

    // Execute the command.
    return bsci.getExecutionQueue().executeShellCommand(context, args);
  }
};

class ShellTool : public Tool {
  BuildSystemImpl& system;

public:
  ShellTool(BuildSystemImpl& system, const std::string& name)
      : Tool(name), system(system) {}

  virtual bool configureAttribute(const std::string& name,
                                  const std::string& value) override {
    system.error(system.getMainFilename(),
                 "unexpected attribute: '" + name + "'");

    // No supported attributes.
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(
      const std::string& name) override {
    return std::make_unique<ShellCommand>(system, name);
  }
};

#pragma mark - ClangTool implementation

class ClangShellCommand : public ExternalCommand {
  /// The compiler command to invoke.
  std::string args;
  
  /// The path to the dependency output file, if used.
  std::string depsPath;
  
  virtual uint64_t getSignature() override {
    uint64_t result = ExternalCommand::getSignature();
    result ^= basic::hashString(args);
    return result;
  }

  bool processDiscoveredDependencies(BuildSystemCommandInterface& bsci,
                                      Task* task,
                                      QueueJobContext* context) {
    // Read the dependencies file.
    auto res = llvm::MemoryBuffer::getFile(depsPath);
    if (auto ec = res.getError()) {
      getBuildSystem(bsci.getBuildEngine()).error(
          depsPath, "unable to open dependencies file (" + ec.message() + ")");
      return false;
    }
    std::unique_ptr<llvm::MemoryBuffer> input(res->release());

    // Parse the output.
    //
    // We just ignore the rule, and add any dependency that we encounter in the
    // file.
    struct DepsActions : public core::MakefileDepsParser::ParseActions {
      BuildSystemCommandInterface& bsci;
      Task* task;
      ClangShellCommand* command;
      unsigned numErrors{0};

      DepsActions(BuildSystemCommandInterface& bsci, Task* task,
                  ClangShellCommand* command)
          : bsci(bsci), task(task), command(command) {}

      virtual void error(const char* message, uint64_t position) override {
        getBuildSystem(bsci.getBuildEngine()).error(
            command->depsPath,
            "error reading dependency file: " + std::string(message));
        ++numErrors;
      }

      virtual void actOnRuleDependency(const char* dependency,
                                       uint64_t length) override {
        bsci.taskDiscoveredDependency(
            task, BuildKey::makeNode(llvm::StringRef(dependency, length)));
      }

      virtual void actOnRuleStart(const char* name, uint64_t length) override {}
      virtual void actOnRuleEnd() override {}
    };

    DepsActions actions(bsci, task, this);
    core::MakefileDepsParser(input->getBufferStart(), input->getBufferSize(),
                             actions).parse();
    return actions.numErrors == 0;
  }

public:
  using ExternalCommand::ExternalCommand;
  
  virtual bool configureAttribute(const std::string& name,
                                  const std::string& value) override {
    if (name == "args") {
      args = value;
    } else if (name == "deps") {
      depsPath = value;
    } else {
      return ExternalCommand::configureAttribute(name, value);
    }

    return true;
  }

  virtual bool executeExternalCommand(BuildSystemCommandInterface& bsci,
                                      Task* task,
                                      QueueJobContext* context) override {
    // Log the command.
    //
    // FIXME: Design the logging and status output APIs.
    if (getDescription().empty()) {
      fprintf(stdout, "%s\n", args.c_str());
    } else {
      fprintf(stdout, "%s\n", getDescription().c_str());
    }
    fflush(stdout);

    // Execute the command.
    if (!bsci.getExecutionQueue().executeShellCommand(context, args)) {
      // If the command failed, there is no need to gather dependencies.
      return false;
    }

    // Otherwise, collect the discovered dependencies, if used.
    if (!depsPath.empty()) {
      if (!processDiscoveredDependencies(bsci, task, context)) {
        // If we were unable to process the dependencies output, report a
        // failure.
        return false;
      }
    }

    return true;
  }
};

class ClangTool : public Tool {
  BuildSystemImpl& system;

public:
  ClangTool(BuildSystemImpl& system, const std::string& name)
      : Tool(name), system(system) {}

  virtual bool configureAttribute(const std::string& name,
                                  const std::string& value) override {
    system.error(system.getMainFilename(),
                 "unexpected attribute: '" + name + "'");

    // No supported attributes.
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(
      const std::string& name) override {
    return std::make_unique<ClangShellCommand>(system, name);
  }
};

#pragma mark - BuildSystemFileDelegate

BuildSystemDelegate& BuildSystemFileDelegate::getSystemDelegate() {
  return system.getDelegate();
}

void BuildSystemFileDelegate::setFileContentsBeingParsed(
    llvm::StringRef buffer) {
  getSystemDelegate().setFileContentsBeingParsed(buffer);
}

void BuildSystemFileDelegate::error(const std::string& filename,
                                    const Token& at,
                                    const std::string& message) {
  // Delegate to the system delegate.
  auto atSystemToken = BuildSystemDelegate::Token{at.start, at.length};
  system.error(filename, atSystemToken, message);
}

bool
BuildSystemFileDelegate::configureClient(const std::string& name,
                                         uint32_t version,
                                         const property_list_type& properties) {
  // The client must match the configured name of the build system.
  if (name != getSystemDelegate().getName())
    return false;

  // The client version must match the configured version.
  //
  // FIXME: We should give the client the opportunity to support a previous
  // schema version (auto-upgrade).
  if (version != getSystemDelegate().getVersion())
    return false;

  return true;
}

std::unique_ptr<Tool>
BuildSystemFileDelegate::lookupTool(const std::string& name) {
  // First, give the client an opportunity to create the tool.
  auto tool = getSystemDelegate().lookupTool(name);
  if (tool)
    return std::move(tool);

  // Otherwise, look for one of the builtin tool definitions.
  if (name == "shell") {
    return std::make_unique<ShellTool>(system, name);
  } else if (name == "phony") {
    return std::make_unique<PhonyTool>(system, name);
  } else if (name == "clang") {
    return std::make_unique<ClangTool>(system, name);
  }

  return nullptr;
}

void BuildSystemFileDelegate::loadedTarget(const std::string& name,
                                           const Target& target) {
}

void BuildSystemFileDelegate::loadedCommand(const std::string& name,
                                            const Command& command) {
}

std::unique_ptr<Node>
BuildSystemFileDelegate::lookupNode(const std::string& name,
                                    bool isImplicit) {
  return system.lookupNode(name, isImplicit);
}

}

#pragma mark - BuildSystem

BuildSystem::BuildSystem(BuildSystemDelegate& delegate,
                         const std::string& mainFilename)
    : impl(new BuildSystemImpl(*this, delegate, mainFilename))
{
}

BuildSystem::~BuildSystem() {
  delete static_cast<BuildSystemImpl*>(impl);
}

BuildSystemDelegate& BuildSystem::getDelegate() {
  return static_cast<BuildSystemImpl*>(impl)->getDelegate();
}

bool BuildSystem::attachDB(const std::string& path,
                                std::string* error_out) {
  return static_cast<BuildSystemImpl*>(impl)->attachDB(path, error_out);
}

bool BuildSystem::enableTracing(const std::string& path,
                                std::string* error_out) {
  return static_cast<BuildSystemImpl*>(impl)->enableTracing(path, error_out);
}

bool BuildSystem::build(const std::string& name) {
  return static_cast<BuildSystemImpl*>(impl)->build(name);
}
