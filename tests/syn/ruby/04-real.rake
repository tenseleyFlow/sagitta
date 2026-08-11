#!/usr/bin/env jruby
module Build
  extend Tasks
  def self.install
    File.open("artifact") { |f| f.write("ok") }
  end
end
