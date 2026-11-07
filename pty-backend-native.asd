(defsystem "pty-backend-native"
  :version "0.1.0"
  :description "pty-protocol backend: forkpty-style spawn on Unix, ConPTY on Windows (small C library)"
  :author "egao1980"
  :license "MIT"
  :depends-on ("pty-protocol" "cffi" "uiop" "trivial-garbage")
  :serial t
  :pathname "src"
  :components ((:file "package")
               (:file "loader")
               (:file "ffi")
               (:file "backend"))
  :in-order-to ((test-op (test-op "pty-backend-native/tests")))
  :properties
  (:cl-repo
   (:cffi-libraries ("libpty-cl")
    :provides ("pty-backend-native")
    :overlays
    ((:platform (:os "linux" :arch "amd64")
      :layers ((:role "native-library"
                :files (("lib/linux-amd64/libpty-cl.so" . "libpty-cl.so")))))
     (:platform (:os "linux" :arch "arm64")
      :layers ((:role "native-library"
                :files (("lib/linux-arm64/libpty-cl.so" . "libpty-cl.so")))))
     (:platform (:os "darwin" :arch "arm64")
      :layers ((:role "native-library"
                :files (("lib/darwin-arm64/libpty-cl.dylib" . "libpty-cl.dylib")))))
     (:platform (:os "windows" :arch "amd64")
      :layers ((:role "native-library"
                :files (("lib/windows-amd64/pty-cl.dll" . "pty-cl.dll")))))))))

(defsystem "pty-backend-native/tests"
  :depends-on ("pty-backend-native" "rove")
  :pathname "tests"
  :serial t
  :components ((:file "package")
               (:file "backend-test"))
  :perform (test-op (o c)
             (unless (symbol-call :rove :run c)
               (error "tests failed for ~A" (component-name c)))))
