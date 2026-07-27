// Standalone, strictly READ-ONLY diagnostic: connects to every detected GigE
// camera and reports whether specific GenICam nodes exist/are writable and,
// for TriggerSelector, exactly which enum entries it currently accepts.
//
// Written to answer one question independently of Mosaic's own probing code
// in video_grabber.cpp (see gige_action_command.hpp's Action Command support):
// does this camera's firmware really lack Action Command support, or does it
// merely lack the "AcquisitionStart" TriggerSelector entry our probe happens
// to try first? This tool never calls SetValue()/Execute()/SetToMinimum() on
// any node — only IsAvailable()/IsWritable()/GetValue()/GetEntries() — so it
// is safe to run at any time, including against cameras already in use.

#define NOMINMAX
#include <pylon/PylonIncludes.h>

#include <iostream>
#include <vector>

namespace {

// Reports availability (and writability, for parameters that can be written)
// of a single named node, without ever writing to it.
void report_node(GenApi::INodeMap& nodemap, const char* name) {
    GenApi::INode* node = nodemap.GetNode(name);
    if (!node) {
        std::cout << "    " << name << ": NOT PRESENT\n";
        return;
    }
    const bool available = GenApi::IsAvailable(node);
    const bool writable  = GenApi::IsWritable(node);
    std::cout << "    " << name << ": "
              << (available ? "available" : "NOT available")
              << ", " << (writable ? "writable" : "not writable") << "\n";
}

// Reports availability of an enum node plus every symbolic entry it
// currently accepts (its actual writable value set, not just a name check).
void report_enum_entries(GenApi::INodeMap& nodemap, const char* name) {
    GenApi::INode* node = nodemap.GetNode(name);
    if (!node) {
        std::cout << "    " << name << ": NOT PRESENT\n";
        return;
    }
    GenApi::CEnumerationPtr enumNode(node);
    if (!enumNode.IsValid()) {
        std::cout << "    " << name << ": present but not an enumeration node\n";
        return;
    }
    const bool available = GenApi::IsAvailable(node);
    std::cout << "    " << name << ": " << (available ? "available" : "NOT available")
              << ", entries = { ";
    GenApi::StringList_t symbolics;
    enumNode->GetSymbolics(symbolics);
    for (size_t i = 0; i < symbolics.size(); ++i) {
        std::cout << symbolics[i].c_str();
        if (i + 1 < symbolics.size()) { std::cout << ", "; }
    }
    std::cout << " }\n";
}

} // namespace

int main() {
    Pylon::PylonAutoInitTerm autoInitTerm; // matches VideoGrabber's discipline: init once, terminate on scope exit

    try {
        Pylon::DeviceInfoList_t deviceList;
        Pylon::CTlFactory::GetInstance().EnumerateDevices(deviceList);

        if (deviceList.empty()) {
            std::cout << "No GigE cameras detected.\n";
            return 0;
        }

        std::cout << "Found " << deviceList.size() << " camera(s).\n\n";

        for (size_t i = 0; i < deviceList.size(); ++i) {
            const auto& info = deviceList[i];
            std::cout << "=== Camera " << i << " — serial " << info.GetSerialNumber()
                      << " — model " << info.GetModelName() << " ===\n";

            try {
                Pylon::CInstantCamera camera(Pylon::CTlFactory::GetInstance().CreateDevice(info));
                camera.Open();
                auto& nodemap = camera.GetNodeMap();

                std::cout << "  TriggerSelector:\n";
                report_enum_entries(nodemap, "TriggerSelector");

                std::cout << "  Action Command nodes:\n";
                report_node(nodemap, "ActionSelector");
                report_node(nodemap, "ActionDeviceKey");
                report_node(nodemap, "ActionGroupKey");
                report_node(nodemap, "ActionGroupMask");

                std::cout << "  PTP:\n";
                report_node(nodemap, "GevIEEE1588");

                camera.Close();
            } catch (const Pylon::GenericException& e) {
                std::cout << "  ERROR opening/querying this camera: " << e.GetDescription() << "\n";
            }

            std::cout << "\n";
        }
    } catch (const Pylon::GenericException& e) {
        std::cerr << "Fatal Pylon error: " << e.GetDescription() << "\n";
        return 1;
    }

    return 0;
}
