just for fun~

官方limit req 模块扩展功能，支持按不同的value指定不同的速度限制

demo:

	limit_proxy_req_zone $arg_srvice $arg_pid zone=one:1m rate=20r/s; #default rate
	server {
		listen       8088;
		server_name  localhost;

		location /test {
			limit_proxy_req zone=one lim_key=xxxx&test rate=1000r/s burst=1200 ;
			limit_proxy_req zone=one lim_key=yyyy&test2 burst=2 ; #rate will be 20r/s
			limit_proxy_req zone=one lim_key=zzzz&testing rate=2000r/s burst=2200;
			limit_proxy_req zone=one lim_key=* burst=2200;
		}
	}

usage:

	Syntax:	limit_proxy_req_zone key zone=name:size rate=rate;
	Default:	—
	Context:	http

	key：从url中获取的参数，可以设置多个。
    zone:为共享内存名字，size为内存大小，
    rate: 为默认的限速值，如果limit_proxy_req 没设置rate的话，则改值生效


	Syntax:	limit_proxy_req zone=name lim_key [rate] brust;
	Default:	—
	Context:	location

	zone:共享内存名字，标记从哪个共享内存取数据;
    lim_key：需要限速的关键字组合，多个关键字用&隔开, "*" 使用默认策略，必须放到最后
	rate：关键字对应的请求平均速度，brust：峰值

    其他指令同limit_req 模块。
	限速算法：if（正在处理请求数 - rate*时间 + 1 ）> brust 则返回503。



具体限流逻辑：
配置示例：
limit_req_zone $binary_remote_addr zone=one:10m rate=100r/s;
location /search/ {
        limit_req zone=one burst=5;
}
解释：rate=100r/s;   每10毫秒（rate/1000）生成一个令牌；burst=5，意思是最多能积攒5个令牌备用。

上述配置几个场景的效果：
1、平时速率小于100r/s， 瞬时来了5个请求，因为令牌桶有备用令牌所以每个请求都可以获取一个令牌，结果：放行。
2、平时速率小于100r/s，瞬时来了10个请求，因为令牌桶只有5个令牌，获取令牌的5个请求放行，其他拒绝。
3、平时速度等于100r/s，意味着令牌桶一直时空的（产生一个消费一个）。10毫秒内只能放行一个请求，其余拒绝。
4、平时速率大于100r/s，多于100部分拒绝。
5、速率不稳定（100+r/s，100-r/s，100+r/s，100-r/s），100+的时候能否全部放行取决于100-的时候令牌桶攒了多少令牌。
6、1秒内速度也不均匀，令牌桶无令牌时拒绝。
