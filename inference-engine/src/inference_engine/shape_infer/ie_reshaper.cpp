// Copyright (C) 2018 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <map>
#include <vector>
#include <string>
#include <memory>
#include <tuple>
#include <ie_layers.h>
#include <graph_tools.hpp>
#include <debug.h>

#include "shape_infer/built-in/ie_built_in_holder.hpp"
#include "shape_infer/ie_reshaper.hpp"
#include "details/caseless.hpp"
#include "details/ie_cnn_network_tools.h"
#include "ie_reshaper.hpp"

using namespace InferenceEngine;
using namespace InferenceEngine::details;
using namespace ShapeInfer;

Reshaper::Reshaper(const Context &context, Network::Ptr& network): ctx(context), network(network) {}

Reshaper::Reshaper(ICNNNetwork& network, const LauncherCreator::Ptr& launcherCreator) {
    auto builtIn = std::make_shared<BuiltInShapeInferHolder>();
    _allTypes = getTypeNamesFromExtension(builtIn);
    _extensions.push_back(builtIn);

    auto inputLayers = CNNNetGetAllInputLayers(network);
    for (const auto& layer : inputLayers) {
        _inputLayers.insert(layer);
    }

    _allSortedLayers = CNNNetSortTopologically(network);
    if (_inputLayers.empty() || _allSortedLayers.empty())
        THROW_IE_EXCEPTION << "Unsupported model for shape inference: failed to collect inputs and layers";
    for (auto const& currentLayer : _allSortedLayers) {
        auto foundInput = std::find_if(_inputLayers.begin(), _inputLayers.end(),
                                       [&currentLayer](const CNNLayerPtr& inputLayer) {
                                           return currentLayer->name == inputLayer->name;
                                       });
        ReshapeLauncher::Ptr createdLauncher;
        if (foundInput == _inputLayers.end()) {
            createdLauncher = launcherCreator->createNotInputLauncher(currentLayer.get(), _extensions);
        } else {
            createdLauncher = launcherCreator->createInputLauncher(currentLayer.get(), _extensions);
        }
        _launchers.insert(createdLauncher);
    }
}

void Reshaper::AddExtension(const IShapeInferExtensionPtr& extension) {
    if (!extension) THROW_IE_EXCEPTION << "Failed to add empty shape infer extension";

    if (network) {
        ctx.addExtension(extension);
        return;
    }

    auto newLayerTypes = getTypeNamesFromExtension(extension);
    std::string badLayerTypes;
    for (const auto& type : newLayerTypes) {
        auto ret = _allTypes.insert(type);
        if (!ret.second) {
            if (!badLayerTypes.empty())
                badLayerTypes += ", ";
            badLayerTypes += type;
        }
    }
    if (!badLayerTypes.empty())
        THROW_IE_EXCEPTION << "Failed to add extension with already registered types:" << badLayerTypes;

    for (auto const& layerType : newLayerTypes) {
        auto foundLauncher = _launchers.begin();
        // find all layers with given type
        std::vector<ReshapeLauncher::Ptr> launchersToInsert;
        while (foundLauncher != _launchers.end()) {
            foundLauncher = std::find_if(foundLauncher, _launchers.end(),
                                         [&layerType](const ReshapeLauncher::Ptr& launcher) {
                                             return layerType == launcher->getLayerType();
                                         });
            if (foundLauncher != _launchers.end()) {
                IShapeInferImpl::Ptr impl;
                StatusCode sts = extension->getShapeInferImpl(impl, layerType.c_str(), nullptr);
                if (sts == OK && impl != nullptr) {
                    auto newLauncher = std::make_shared<ReshapeLauncher>((*foundLauncher)->getLayer(), impl);
                    newLauncher->setShapeInferImpl(impl);
                    launchersToInsert.push_back(newLauncher);
                    foundLauncher = _launchers.erase(foundLauncher);
                } else {
                    THROW_IE_EXCEPTION << "Failed to get registered Shape Infer Implementation for type: " << layerType;
                }
            }
        }
        for (const auto& launcher : launchersToInsert) {
            _launchers.insert(launcher);
        }
    }
    _extensions.push_back(extension);
}

ReshapeLauncher::Ptr Reshaper::getLauncherByLayerName(const std::string& layerName) const {
    auto foundLauncher = std::find_if(_launchers.begin(), _launchers.end(),
                                      [&layerName](const ReshapeLauncher::Ptr& launcher) {
                                          return launcher->getLayerName() == layerName;
                                      });
    if (foundLauncher == _launchers.end())
        THROW_IE_EXCEPTION << "Failed to reshape layer ('" << layerName << "'): can't find the corresponding launcher";
    return *foundLauncher;
}

StatusCode Reshaper::run(const std::map<std::string, SizeVector>& inputShapes, ResponseDesc* resp) {
    if (network) {
        return networkShapeInfer(inputShapes, resp);
    }
    // Reset all shapes from previous run
    for (const auto& launcher : _launchers) {
        launcher->reset();
    }

    // Set new input shapes
    for (auto const& input : _inputLayers) {
        std::string layerName = input->name;
        for (auto const& outData : input->outData) {
            std::string dataName = outData->name;
            auto foundShapeIt = inputShapes.find(dataName);
            auto foundLauncher = getLauncherByLayerName(layerName);
            if (foundShapeIt != inputShapes.end()) {
                foundLauncher->setShapeByName(foundShapeIt->second, dataName);
            } else {
                foundLauncher->setIRShapeByName(dataName);
            }
        }
    }

    // do reshape
    for (auto& layer : _allSortedLayers) {
        auto foundLauncher = getLauncherByLayerName(layer->name);
        foundLauncher->reshape(_launchers);
    }

    // apply changes
    for (auto& layer : _allSortedLayers) {
        auto foundLauncher = getLauncherByLayerName(layer->name);
        foundLauncher->applyChanges(layer.get());
    }
    return OK;
}

StatusCode Reshaper::networkShapeInfer(const std::map<std::string, SizeVector>& inputShapes, ResponseDesc* resp) {
    if (!network)
        return DescriptionBuffer(GENERAL_ERROR, resp) << "Cannot infer shapes! Network is not loaded.";
    std::vector<Layer> propagatedLayers;
    Network propagatedNetwork(*network);

    // Set new input shapes
    for (auto& layer : propagatedNetwork) {
        if (inputShapes.find(layer->getName()) == inputShapes.end() ||
                details::CaselessEq<std::string>()(layer->getType(), "Const"))
            continue;

        if (layer->getOutputPorts().size() != 1)
            return DescriptionBuffer(GENERAL_ERROR, resp) << "Cannot infer shapes! Input layers can have only one output port.";

        layer->getOutputPorts()[0].shape() = inputShapes.find(layer->getName())->second;
    }

    // Try to propagate shapes
    for (auto& layer : propagatedNetwork) {
        const auto impl = ctx.getShapeInferImpl(layer->getType());
        if (!impl)
            return DescriptionBuffer(NOT_FOUND, resp) <<
                        "Cannot infer shapes! Shape infer implementation was not found for type " << layer->getType() << ".";
        std::vector<SizeVector> inShapes;
        std::vector<SizeVector> outShapes;
        std::map<std::string, std::string> params;
        std::map<std::string, Blob::Ptr> blobs;

        for (const auto& inPort : layer->getInputPorts().empty() ? layer->getOutputPorts() : layer->getInputPorts()) {
            inShapes.push_back(inPort.shape());
        }
        if (layer->getParameters()) {
            for (const auto& it  : layer->getParameters()->getParameters()) {
                params[it.first] = it.second;
            }
            for (const auto& it  : layer->getParameters()->getConstantData()) {
                blobs[it.first] = std::const_pointer_cast<Blob>(it.second);
            }
        }

        StatusCode sts = impl->inferShapes(inShapes, params, blobs, outShapes, resp);
        if (sts != OK)
            return sts;

        if (outShapes.size() != layer->getOutputPorts().size())
            return DescriptionBuffer(GENERAL_ERROR, resp) << "Cannot infer shapes! The number of output shapes is not equal the number of output ports.";

        for (size_t i = 0; i < outShapes.size(); i++) {
            layer->getOutputPorts()[i].shape() = outShapes[i];
        }
        for (const auto& connection : propagatedNetwork.getLayerConnections(layer->getId())) {
            if (connection.from().layerId() != layer->getId())
                continue;
            auto nextLayer = propagatedNetwork.getLayer(connection.to().layerId());
            nextLayer->getInputPorts()[connection.to().portId()].shape() = outShapes[connection.from().portId()];
        }
    }

    // Apply new shapes
    for (auto& layer : *network) {
        const auto& propagatedLayer = propagatedNetwork.getLayer(layer->getId());
        for (size_t i = 0; i < layer->getInputPorts().size(); i++) {
            layer->getInputPorts()[i].shape() = propagatedLayer->getInputPorts()[i].shape();
        }
        for (size_t i = 0; i < layer->getOutputPorts().size(); i++) {
            layer->getOutputPorts()[i].shape() = propagatedLayer->getOutputPorts()[i].shape();
        }
    }
    return OK;
}

caseless_set<std::string> Reshaper::getTypeNamesFromExtension(const IShapeInferExtensionPtr& extension) {
    char** types = nullptr;
    unsigned int size = 0;
    ResponseDesc resp;
    StatusCode sts = extension->getShapeInferTypes(types, size, &resp);
    if (sts != OK) THROW_IE_EXCEPTION << "Failed to get types from extension: " << resp.msg;
    caseless_set<std::string> typesSet;
    for (int i = 0; i < size; i++) {
        std::string type(types[i], strlen(types[i]));
        delete[] types[i];
        typesSet.insert(type);
    }
    delete[] types;
    return typesSet;
}

ReshapeLauncher::Ptr
LauncherCreator::createNotInputLauncher(const CNNLayer* layer, const std::vector<IShapeInferExtensionPtr>& extensions) {
    auto layerType = layer->type;
    if ((::details::equal(layerType, "memory") && layer->GetParamAsInt("index")) ||
        ::details::equal(layerType, "const") || ::details::equal(layerType, "input")) {
        THROW_IE_EXCEPTION << "Failed to reshape: Layer with type `" << layerType
                           << "` can't be intermediate layer in network";
    }

    for (const auto& extension : extensions) {
        IShapeInferImpl::Ptr impl = nullptr;
        StatusCode sts = extension->getShapeInferImpl(impl, layerType.c_str(), nullptr);
        if (sts == OK && impl != nullptr) {
            if (::details::equal(layerType, "memory") && !layer->GetParamAsInt("index")) {
                return std::make_shared<OutMemoryReshapeLauncher>(layer, nullptr);
            }
            return std::make_shared<ReshapeLauncher>(layer, impl);
        }
    }
    return std::make_shared<FakeReshapeLauncher>(layer, nullptr);
}

ReshapeLauncher::Ptr
LauncherCreator::createInputLauncher(const CNNLayer* layer, const std::vector<IShapeInferExtensionPtr>& extensions) {
    auto layerType = layer->type;
    if (::details::equal(layerType, "memory") && layer->GetParamAsInt("index")) {
        return std::make_shared<InputReshapeLauncher>(layer, nullptr);
    } else if (::details::equal(layerType, "const")) {
        return std::make_shared<ConstReshapeLauncher>(layer, nullptr);
    } else if (::details::equal(layerType, "input")) {
        return std::make_shared<InputReshapeLauncher>(layer, nullptr);
    }
    THROW_IE_EXCEPTION << "Failed to reshape: Layer with type `" << layerType
                       << "` can't be input. Supported input types: Input, Const and Memory(with index=1)";
}

