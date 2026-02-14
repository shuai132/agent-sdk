# Qwen OAuth 认证集成指南

本文档介绍了如何在 agent-sdk 中使用阿里云通义千问（Qwen）API 的 OAuth 认证方式。

## 目录
1. [概述](#概述)
2. [OAuth 认证流程](#oauth-认证流程)
3. [实现细节](#实现细节)
4. [使用方法](#使用方法)
5. [示例代码](#示例代码)

## 概述

agent-sdk 现在支持使用 OAuth 2.0 认证方式访问 Qwen API。除了传统的 API Key 认证外，您还可以使用 OAuth 令牌进行身份验证。

## OAuth 认证流程

Qwen API 的 OAuth 认证遵循标准的 OAuth 2.0 授权码流程：

1. **授权请求**：应用程序重定向用户到授权服务器
2. **用户认证**：用户在授权服务器上登录并授权应用
3. **授权码返回**：授权服务器重定向用户回应用，并附带授权码
4. **令牌交换**：应用使用授权码换取访问令牌
5. **API 调用**：应用使用访问令牌调用 API

## 实现细节

### QwenProvider 类

`QwenProvider` 类实现了 `Provider` 接口，支持：
- 标准 API Key 认证
- OAuth 令牌认证（通过自定义 Authorization 头）

### QwenOAuthHelper 类

`QwenOAuthHelper` 类提供了以下功能：
- `initiate_oauth_flow()` - 生成授权 URL
- `exchange_code_for_token()` - 用授权码换取访问令牌
- `refresh_access_token()` - 使用刷新令牌更新访问令牌
- `validate_token()` - 验证访问令牌的有效性

## 使用方法

### 1. 初始化 OAuth 流程

```cpp
std::string auth_url = QwenOAuthHelper::initiate_oauth_flow(
    "your-client-id", 
    "your-redirect-uri",
    "api_invoke"
);
```

### 2. 交换授权码获取令牌

```cpp
auto access_token = QwenOAuthHelper::exchange_code_for_token(
    client_id, 
    client_secret, 
    auth_code, 
    redirect_uri
);
```

### 3. 创建使用 OAuth 的 Qwen 提供商

```cpp
ProviderConfig config;
config.name = "qwen";
config.api_key = access_token;  // 存储 OAuth 令牌作为 api_key
config.base_url = "https://dashscope.aliyuncs.com";

// 添加 Authorization 头以使用 OAuth 令牌
config.headers["Authorization"] = "Bearer " + access_token;

auto provider = std::make_shared<QwenProvider>(config, io_ctx);
```

## 示例代码

完整的示例代码可在 `examples/qwen_oauth_example.cpp` 中找到。

该示例演示了：
1. 使用传统 API Key 方式调用 Qwen
2. 使用 OAuth 认证方式调用 Qwen
3. 刷新 OAuth 访问令牌

要运行示例，请确保设置了正确的客户端 ID、密钥和重定向 URI：

```bash
# 首先构建项目
cmake --build build --target agent_sdk_qwen_oauth_example

# 运行示例
./build/agent_sdk_qwen_oauth_example
```

## 注意事项

1. **安全性**：确保客户端密钥和其他敏感信息得到妥善保护
2. **令牌存储**：访问令牌和刷新令牌应安全存储
3. **令牌过期**：注意访问令牌的过期时间，及时使用刷新令牌更新
4. **错误处理**：实现适当的错误处理逻辑以应对认证失败情况

## 阿里云控制台设置

要在阿里云上设置 OAuth 认证：

1. 访问 [阿里云控制台](https://home.console.aliyun.com/)
2. 导航到 "访问控制" 或 "API 市场"
3. 创建新的应用并获取客户端 ID 和密钥
4. 配置重定向 URI
5. 获取相应的 API 调用权限