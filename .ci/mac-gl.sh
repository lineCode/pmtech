#!/usr/bin/env bash
cd examples
../pmbuild mac-gl -libs
../pmbuild mac-gl
../pmbuild make mac-gl all -configuration Debug -quiet
../pmbuild make mac-gl all -configuration Release -quiet