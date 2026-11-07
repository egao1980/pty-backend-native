(in-package #:pty-backend-native)

(defconstant +abi+ 1)

(cffi:defcfun ("pty_cl_abi" %abi) :int)
(cffi:defcfun ("pty_cl_spawn" %spawn) :int
  (file :string) (argv :pointer) (envp :pointer) (cwd :pointer)
  (cols :uint16) (rows :uint16) (out :pointer) (out-errno :pointer))
(cffi:defcfun ("pty_cl_read" %read) :long
  (pty :pointer) (buf :pointer) (len :size) (timeout-ms :int))
(cffi:defcfun ("pty_cl_write" %write) :long
  (pty :pointer) (buf :pointer) (len :size))
(cffi:defcfun ("pty_cl_resize" %resize) :int (pty :pointer) (cols :uint16) (rows :uint16))
(cffi:defcfun ("pty_cl_signal" %signal) :int (pty :pointer) (signal :int))
(cffi:defcfun ("pty_cl_wait" %wait) :int
  (pty :pointer) (timeout-ms :int) (code :pointer) (signal :pointer))
(cffi:defcfun ("pty_cl_pid" %pid) :long (pty :pointer))
(cffi:defcfun ("pty_cl_errno" %errno) :int (pty :pointer))
(cffi:defcfun ("pty_cl_close" %close) :void (pty :pointer))

(let ((abi (%abi)))
  (unless (= abi +abi+)
    (error 'pp:pty-native-unavailable
           :reason (format nil "libpty-cl ABI ~A, expected ~A" abi +abi+))))
