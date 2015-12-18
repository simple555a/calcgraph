#!/usr/bin/env ruby

require 'socket'
require 'timeout'

pid = fork { exec "#{Dir.pwd}/example" }
sock = UDPSocket.new
sleep 0.1

# initial yield curve from  https://www.treasury.gov/resource-center/data-chart-center/interest-rates/Pages/TextView.aspx?data=yield
maturities = [1, 2, 3, 5, 7, 10, 20, 30]
prices = [0.51, 0.91, 1.19, 1.59, 1.93, 2.15, 2.55, 2.91]

maturities.zip(prices).each { |maturity, price|
	sock.send "#{maturity}Y #{price}", 0, "localhost", 8080
}

begin
	Timeout::timeout(30) {
	  loop {
	  	i = Random.rand maturities.size
	  	prices[i] += Random.rand > 0.5 ? 0.01 : -0.01
	  	sock.send "#{maturities[i]}Y #{prices[i]}", 0, "localhost", 8080
	  }
	}
rescue Timeout::Error
end

Process.kill("TERM", pid)
Process.wait(pid)