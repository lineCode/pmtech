#!/usr/bin/env bash
find examples/code -name '**.cpp' -or -name '*.h' | xargs clang-format -style=file -i
find pen/include -name '**.inl' -or -name '*.h' | xargs clang-format -style=file -i
find pen/source -name '**.cpp' -or -name '*.h' | xargs clang-format -style=file -i
find put/include -name '**.inl' -or -name '*.h' | xargs clang-format -style=file -i
find put/source -name '**.cpp' -or -name '*.h' | xargs clang-format -style=file -i