#!/bin/sh
# Nombre de plataforma para los artefactos: linux-x64, macos-arm64, windows-x64...
case "$(uname -s)" in
  Darwin) case "$(uname -m)" in
           arm64|aarch64) echo macos-arm64 ;;
           *) echo macos-x64 ;;
         esac ;;
  Linux) case "$(uname -m)" in
          aarch64|arm64) echo linux-arm64 ;;
          *) echo linux-x64 ;;
        esac ;;
  MINGW*|MSYS*|CYGWIN*) case "$(uname -m)" in
                        x86_64) echo windows-x64 ;;
                        *) echo windows-arm64 ;;
                      esac ;;
  *) echo "desconocida" ;;
esac
