// Host harness for the DXBC -> GLSL translator.
//
// There is no way to run a Quest from here and no DirectX compiler on this
// host, so the translator is exercised against hand-assembled bytecode (see
// mkdxbc.py) through this tool. Every mode prints one field per line so the
// test harness never has to guess where a value ends.
//
// Build (matching CI):
//   g++ -std=gnu++20 -fsanitize=address,undefined -g -I include \
//       tools/dxbc/main.cpp src/VivifyDxbc.cpp -o /tmp/dxbctool
//
// Modes:
//   dxbctool parse  <file>   container, signatures, reflection
//   dxbctool disasm <file>   one line per decoded instruction
//   dxbctool glsl   <file>   translated GLSL ES, or the reason there is none
#include "VivifyDxbc.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> ReadFile(char const* path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
}

void PrintProgram(Vivify::Dxbc::Program const& program) {
  std::cout << "ok=" << (program.ok ? 1 : 0) << "\n";
  if (!program.error.empty()) std::cout << "error=" << program.error << "\n";
  if (!program.ok) return;
  std::cout << "stage=" << Vivify::Dxbc::StageName(program.stage) << "\n";
  std::cout << "version=" << program.majorVersion << "." << program.minorVersion << "\n";
  std::cout << "containerOffset=" << program.containerOffset << "\n";
  std::cout << "instructions=" << program.instructions.size() << "\n";
  std::cout << "temps=" << program.tempCount << "\n";
  std::cout << "creator=" << program.creator << "\n";
  for (auto const& element : program.inputSignature) {
    std::cout << "input=" << element.semanticName << " index=" << element.semanticIndex
              << " reg=" << element.registerIndex << " sv=" << element.systemValueType
              << " mask=" << static_cast<int>(element.mask) << "\n";
  }
  for (auto const& element : program.outputSignature) {
    std::cout << "output=" << element.semanticName << " index=" << element.semanticIndex
              << " reg=" << element.registerIndex << " sv=" << element.systemValueType
              << " mask=" << static_cast<int>(element.mask) << "\n";
  }
  for (auto const& buffer : program.constantBuffers) {
    std::cout << "cbuffer=" << buffer.name << " size=" << buffer.size
              << " bind=" << buffer.bindPoint << " variables=" << buffer.variables.size() << "\n";
    for (auto const& variable : buffer.variables) {
      std::cout << "cbvar=" << variable.name << " offset=" << variable.startOffset
                << " size=" << variable.size << " class=" << variable.variableClass
                << " type=" << variable.variableType << " rows=" << variable.rows
                << " columns=" << variable.columns << " elements=" << variable.elements << "\n";
    }
  }
  for (auto const& binding : program.resourceBindings) {
    std::cout << "binding=" << binding.name << " type=" << binding.type
              << " dimension=" << binding.dimension << " bind=" << binding.bindPoint << "\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: dxbctool <parse|disasm|glsl> <file>\n";
    return 2;
  }
  std::string const mode = argv[1];
  std::vector<uint8_t> const data = ReadFile(argv[2]);

  Vivify::Dxbc::Program const program =
      Vivify::Dxbc::ParseProgram(data.empty() ? nullptr : data.data(), data.size());

  if (mode == "parse") {
    PrintProgram(program);
    return 0;
  }
  if (mode == "disasm") {
    std::cout << "ok=" << (program.ok ? 1 : 0) << "\n";
    if (!program.error.empty()) std::cout << "error=" << program.error << "\n";
    if (program.ok) std::cout << Vivify::Dxbc::DisassembleProgram(program);
    return 0;
  }
  if (mode == "glsl") {
    Vivify::Dxbc::GlslResult const result = Vivify::Dxbc::TranslateToGlsl(program);
    std::cout << "ok=" << (result.ok ? 1 : 0) << "\n";
    if (!result.error.empty()) std::cout << "error=" << result.error << "\n";
    for (auto const& name : result.uniforms) std::cout << "uniform=" << name << "\n";
    for (auto const& name : result.samplers) std::cout << "sampler=" << name << "\n";
    if (result.ok) {
      std::cout << "---\n";
      std::cout << result.source;
    }
    return 0;
  }
  std::cerr << "unknown mode: " << mode << "\n";
  return 2;
}
