(asdf:defsystem "std-slop"
  :description "Lisp implementation of std::slop"
  :version "0.1.0"
  :author "hsaliak <hsaliak@gmail.com>"
  :license "MIT"
  :depends-on ("cl-sqlite"
               "drakma"
               "jonathan"
               "command-line-arguments"
               "log4cl"
               "uiop")
  :serial t
  :components ((:file "package")
               (:file "main")))
