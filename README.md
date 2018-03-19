just for fun~

demo:

	limit_req_zone $arg_srvice $arg_pid zone=one:1m rate=20r/s; #default rate
	server {
		listen       8088;
		server_name  localhost;

		location /test {
			limit_req zone=one lim_key=xxxx&test rate=1000r/s burst=1200 ;
			limit_req zone=one lim_key=yyyy&test2 burst=2 ; #rate will be 20r/s
			limit_req zone=one lim_key=zzzz&testing rate=2000r/s burst=2200;
			
		}
	}

usage:

	Syntax:	limit_req_zone key zone=name:size rate=rate;
	Default:	—
	Context:	http

	key：从url中获取的参数，可以设置多个。 zone为共享内存名字，size为内存大小，rate 为默认的限速值，如果limit_req 没设置
	rate的话，则改值生效


	Syntax:	limit_req zone=name lim_key [rate] brust;
	Default:	—
	Context:	location

	zone:共享内存名字，标记从哪个共享内存取数据; lim_key：需要限速的关键字组合，多个关键字用&隔开；
	rate：关键字对应的请求平均速度，brust：峰值

	限速算法：if（正在处理请求数 - rate*时间 + 1 ）> brust 则返回503。
