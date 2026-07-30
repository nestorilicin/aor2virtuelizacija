all:
	$(MAKE) -C host
	$(MAKE) -C guest

clean:
	$(MAKE) -C host clean
	$(MAKE) -C guest clean
