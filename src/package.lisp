(defpackage #:pty-backend-native
  (:use #:cl)
  (:local-nicknames (#:pp #:pty-protocol))
  (:export #:native-backend
           #:native-pty))
