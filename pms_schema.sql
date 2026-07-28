-- =============================================================
-- 新一代设备资产精益管理系统 (PMS) MySQL Schema
-- 生成来源: pms-SQL.csv 中全部 SQL 语句逆向分析
-- 数据库划分说明:
--   power_common      作业资源 - 公共组织机构 (简版)
--   power_sch         作业资源 - 装备/项目管理
--   middleground_public  中台 - 公共数据 (组织、用户、代码表)
--   middleground_ast  中台 - 资产管理
--   middleground_psr  中台 - 电力系统资源
--   pms               生产作业 - 业务主库 (无前缀表)
-- =============================================================

-- ============================================================
-- Database: power_common
-- 作业资源管理 - 简版公共组织机构
-- ============================================================
CREATE DATABASE IF NOT EXISTS power_common DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE power_common;

CREATE TABLE t_public_organization (
    org_id          VARCHAR(64)  NOT NULL COMMENT '组织机构ID',
    org_name        VARCHAR(200) NOT NULL COMMENT '组织机构名称',
    org_level       VARCHAR(10)           COMMENT '组织层级',
    full_path_id    VARCHAR(500)          COMMENT '全路径ID（用于 LIKE 模糊查询下级）',
    PRIMARY KEY (org_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='公共组织机构表（power_common 简版）';


-- ============================================================
-- Database: power_sch
-- 作业资源管理 - 装备与项目
-- ============================================================
CREATE DATABASE IF NOT EXISTS power_sch DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE power_sch;

-- 设备主数据表
CREATE TABLE t_equipment_master_data (
    id                VARCHAR(64)  NOT NULL COMMENT '设备ID（主键）',
    equipment_state   VARCHAR(20)           COMMENT '设备状态',
    equipment_category VARCHAR(50)          COMMENT '设备大类',
    equipment_type    VARCHAR(50)           COMMENT '设备类型',
    management_org    VARCHAR(64)           COMMENT '管理单位ID（关联 power_common.t_public_organization.org_id）',
    PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='设备主数据表';

-- 无人机（作业装备）状态表
CREATE TABLE t_ast_wa_drone (
    id          BIGINT       NOT NULL AUTO_INCREMENT COMMENT '自增主键',
    ast_id      VARCHAR(64)  NOT NULL COMMENT '资产ID（关联 t_equipment_master_data.id）',
    deploy_state VARCHAR(20)          COMMENT '部署状态',
    PRIMARY KEY (id),
    INDEX idx_ast_id (ast_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='作业装备（无人机等）状态表';

-- 资产配置表（装备分类配置）
CREATE TABLE t_pa_asset_config (
    id             BIGINT      NOT NULL AUTO_INCREMENT COMMENT '自增主键',
    equip_type     VARCHAR(50)          COMMENT '设备类型',
    classsification VARCHAR(50)         COMMENT '分类（注：原字段名含拼写）',
    full_path_id   VARCHAR(500)         COMMENT '组织全路径ID',
    org_id         VARCHAR(64)          COMMENT '组织ID',
    PRIMARY KEY (id),
    INDEX idx_equip_type (equip_type)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='资产类型配置表';

-- 项目计划表（零购项目）
CREATE TABLE pa_project_pl (
    item_id               VARCHAR(64)   NOT NULL COMMENT '项目条目ID',
    item_org              VARCHAR(64)            COMMENT '项目归属单位ID',
    item_org_name         VARCHAR(200)           COMMENT '项目归属单位名称',
    professional_department VARCHAR(100)         COMMENT '专业部门',
    quantity              DECIMAL(18,4)          COMMENT '数量',
    plan_total_sum        DECIMAL(18,4)          COMMENT '计划金额',
    plan_year             CHAR(4)                COMMENT '计划年度',
    PRIMARY KEY (item_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='项目计划表';

-- 项目设备明细表
CREATE TABLE pa_project_equip_pl (
    id             BIGINT       NOT NULL AUTO_INCREMENT COMMENT '自增主键',
    item_id        VARCHAR(64)  NOT NULL COMMENT '项目条目ID（关联 pa_project_pl.item_id）',
    parent_type_id VARCHAR(50)           COMMENT '父类型ID（设备大类）',
    PRIMARY KEY (id),
    INDEX idx_item_id (item_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='项目设备明细表';


-- ============================================================
-- Database: middleground_public
-- 中台 - 公共数据（组织机构、用户、代码表）
-- ============================================================
CREATE DATABASE IF NOT EXISTS middleground_public DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE middleground_public;

-- 公共组织机构完整表
CREATE TABLE t_public_organization (
    org_id                        VARCHAR(64)   NOT NULL COMMENT '组织机构ID',
    org_name                      VARCHAR(200)  NOT NULL COMMENT '组织名称',
    org_nature                    VARCHAR(50)            COMMENT '组织性质',
    org_code                      VARCHAR(100)           COMMENT '组织编码',
    parent_id                     VARCHAR(64)            COMMENT '上级组织ID',
    display_order                 INT                    COMMENT '显示顺序',
    professional_nature           VARCHAR(50)            COMMENT '专业性质',
    manage_level                  VARCHAR(20)            COMMENT '管理层级',
    org_short_name                VARCHAR(100)           COMMENT '组织简称',
    ctime                         DATETIME               COMMENT '创建时间',
    syn_time                      DATETIME               COMMENT '同步时间',
    repeal_time                   DATETIME               COMMENT '撤销时间',
    is_valid                      TINYINT(1)             COMMENT '是否有效',
    branch_id                     VARCHAR(64)            COMMENT '分支机构ID',
    province                      VARCHAR(64)            COMMENT '所属省级组织ID',
    city                          VARCHAR(64)            COMMENT '所属地市组织ID',
    company                       VARCHAR(64)            COMMENT '所属供电公司ID',
    reserved_id1                  VARCHAR(64)            COMMENT '预留ID1（原系统ID）',
    erp_org_type                  VARCHAR(50)            COMMENT 'ERP组织类型',
    org_level                     VARCHAR(20)            COMMENT '组织层级',
    full_path_name                VARCHAR(1000)          COMMENT '全路径名称',
    full_path_id                  VARCHAR(500)           COMMENT '全路径ID',
    erp_code                      VARCHAR(100)           COMMENT 'ERP编码',
    finance_code                  VARCHAR(100)           COMMENT '财务编码',
    reserved_id3                  VARCHAR(64)            COMMENT '预留ID3（主体性质）',
    maint_org                     VARCHAR(64)            COMMENT '所属运维单位ID',
    org_full_name                 VARCHAR(500)           COMMENT '组织全称',
    parent_name                   VARCHAR(200)           COMMENT '上级组织名称',
    province_name                 VARCHAR(200)           COMMENT '省级名称',
    city_name                     VARCHAR(200)           COMMENT '地市名称',
    company_name                  VARCHAR(200)           COMMENT '供电公司名称',
    maint_org_name                VARCHAR(200)           COMMENT '运维单位名称',
    maint_group_value             VARCHAR(100)           COMMENT '运维班组值',
    is_repeal                     TINYINT(1)             COMMENT '是否撤销',
    city_deploy                   VARCHAR(20)            COMMENT '城市部署标识',
    shortname_pinyin              VARCHAR(200)           COMMENT '简称拼音',
    last_update_time              DATETIME               COMMENT '最后更新时间',
    department_full_name          VARCHAR(500)           COMMENT '部门全称',
    unicode                       VARCHAR(100)           COMMENT '统一编码',
    is_product_org                TINYINT(1)             COMMENT '是否生产性组织',
    base_org_id                   VARCHAR(64)            COMMENT '基础组织ID',
    org_name_pinyin               VARCHAR(500)           COMMENT '组织名称拼音',
    org_name_pinyin_initial       VARCHAR(200)           COMMENT '组织名称拼音首字母',
    org_shortname_pinyin_initial  VARCHAR(200)           COMMENT '简称拼音首字母',
    isc_pms2_write_back           TINYINT(1)             COMMENT '是否写回PMS2',
    leaf                          TINYINT(1)             COMMENT '是否叶子节点',
    PRIMARY KEY (org_id),
    INDEX idx_parent_id (parent_id),
    INDEX idx_full_path_id (full_path_id(255))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='公共组织机构完整表（中台）';

-- 公共代码表
CREATE TABLE t_public_commom_code (
    code_id        BIGINT       NOT NULL AUTO_INCREMENT COMMENT '主键',
    standtype_code VARCHAR(100) NOT NULL COMMENT '标准类型编码',
    code           VARCHAR(100) NOT NULL COMMENT '代码值',
    code_name      VARCHAR(200)          COMMENT '代码名称',
    remark         VARCHAR(500)          COMMENT '备注',
    PRIMARY KEY (code_id),
    INDEX idx_standtype_code (standtype_code),
    INDEX idx_code (code)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='公共代码表';

-- 物料制造商/厂家字典表
CREATE TABLE t_public_stdlib_manufacturer (
    id     BIGINT       NOT NULL AUTO_INCREMENT COMMENT '主键',
    name   VARCHAR(200)          COMMENT '制造商名称',
    PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='标准物料制造商字典表';

-- 用户表
CREATE TABLE t_public_user (
    user_id        VARCHAR(64)   NOT NULL COMMENT '用户ID',
    user_name      VARCHAR(100)           COMMENT '用户姓名',
    login_name     VARCHAR(100)           COMMENT '登录名',
    org_id         VARCHAR(64)            COMMENT '所属组织ID',
    title          VARCHAR(50)            COMMENT '职称',
    post           VARCHAR(50)            COMMENT '岗位',
    professional   VARCHAR(50)            COMMENT '专业',
    gender         VARCHAR(10)            COMMENT '性别',
    interphone     VARCHAR(50)            COMMENT '内线电话',
    department_id  VARCHAR(64)            COMMENT '所属部门ID',
    dispatch_id    VARCHAR(64)            COMMENT '所属调度机构ID',
    dispatch_name  VARCHAR(200)           COMMENT '调度机构名称',
    PRIMARY KEY (user_id),
    INDEX idx_org_id (org_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='中台用户表';


-- ============================================================
-- Database: middleground_ast
-- 中台 - 资产管理
-- ============================================================
CREATE DATABASE IF NOT EXISTS middleground_ast DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE middleground_ast;

-- 主变压器资产表
CREATE TABLE t_ast_tf_maintransformer (
    ast_id        VARCHAR(64)  NOT NULL COMMENT '资产ID',
    deploy_state  VARCHAR(20)           COMMENT '投运状态',
    utc_num       VARCHAR(100)          COMMENT 'UTC编号（赋码标识）',
    voltage_level VARCHAR(20)           COMMENT '电压等级',
    operate_date  DATE                  COMMENT '投运日期',
    maint_org     VARCHAR(64)           COMMENT '运维单位ID',
    PRIMARY KEY (ast_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='主变压器资产表';

-- 备品备件表
CREATE TABLE t_pa_spare_parts (
    spare_parts_id        VARCHAR(64)   NOT NULL COMMENT '备件ID',
    ast_id                VARCHAR(64)            COMMENT '关联资产ID',
    custody_department    VARCHAR(64)            COMMENT '保管单位ID',
    disposal_type         VARCHAR(50)            COMMENT '处置类型',
    manage_org            VARCHAR(64)            COMMENT '管理单位ID',
    manufacturer          VARCHAR(64)            COMMENT '制造商ID',
    storage_location_name VARCHAR(200)           COMMENT '存储地点名称',
    storage_date          DATE                   COMMENT '入库日期',
    spare_category        VARCHAR(50)            COMMENT '备件类别',
    spare_name            VARCHAR(200)           COMMENT '备件名称',
    spare_model           VARCHAR(200)           COMMENT '备件型号',
    spare_num             INT                    COMMENT '备件数量',
    spare_unit            VARCHAR(20)            COMMENT '单位',
    spare_state           VARCHAR(20)            COMMENT '备件状态',
    voltage_level         VARCHAR(20)            COMMENT '电压等级',
    equip_type            VARCHAR(50)            COMMENT '设备类型',
    ctime                 DATETIME               COMMENT '创建时间',
    mtime                 DATETIME               COMMENT '修改时间',
    is_deleted            TINYINT(1) DEFAULT 0   COMMENT '是否删除',
    PRIMARY KEY (spare_parts_id),
    INDEX idx_ast_id (ast_id),
    INDEX idx_manage_org (manage_org)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='备品备件表';

-- 备件配额配置表
CREATE TABLE pa_spare_quota_configure_pl (
    id              BIGINT       NOT NULL AUTO_INCREMENT COMMENT '主键',
    equip_type      VARCHAR(50)           COMMENT '设备类型',
    voltage_level   VARCHAR(20)           COMMENT '电压等级',
    quota_num       INT                   COMMENT '配额数量',
    org_id          VARCHAR(64)           COMMENT '组织ID',
    is_deleted      TINYINT(1) DEFAULT 0  COMMENT '是否删除',
    ctime           DATETIME              COMMENT '创建时间',
    mtime           DATETIME              COMMENT '修改时间',
    PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='备件配额配置表';

-- 备件领用设备明细表
CREATE TABLE pa_spare_claim_equip_pl (
    id          BIGINT       NOT NULL AUTO_INCREMENT COMMENT '主键',
    claim_id    VARCHAR(64)  NOT NULL COMMENT '领用单ID',
    ast_id      VARCHAR(64)           COMMENT '资产ID',
    equip_type  VARCHAR(50)           COMMENT '设备类型',
    equip_name  VARCHAR(200)          COMMENT '设备名称',
    quantity    INT                   COMMENT '领用数量',
    ctime       DATETIME              COMMENT '创建时间',
    PRIMARY KEY (id),
    INDEX idx_claim_id (claim_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='备件领用设备明细表';

-- 备件试验设备表
CREATE TABLE pa_spare_test_equip_pl (
    id             BIGINT       NOT NULL AUTO_INCREMENT COMMENT '主键',
    spare_parts_id VARCHAR(64)  NOT NULL COMMENT '备件ID',
    test_type      VARCHAR(50)           COMMENT '试验类型',
    test_result    VARCHAR(50)           COMMENT '试验结果',
    test_date      DATE                  COMMENT '试验日期',
    ctime          DATETIME              COMMENT '创建时间',
    PRIMARY KEY (id),
    INDEX idx_spare_parts_id (spare_parts_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='备件试验设备表';

-- 技术鉴定明细表
CREATE TABLE pa_tech_appr_pl (
    id             BIGINT       NOT NULL AUTO_INCREMENT COMMENT '主键',
    data_source_id VARCHAR(64)  NOT NULL COMMENT '数据来源ID',
    appr_result    VARCHAR(50)           COMMENT '鉴定结论',
    appr_date      DATE                  COMMENT '鉴定日期',
    ctime          DATETIME              COMMENT '创建时间',
    PRIMARY KEY (id),
    INDEX idx_data_source_id (data_source_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='技术鉴定明细表';

-- 设备退役计划明细表
CREATE TABLE pa_remove_plan_equip_pl (
    id        BIGINT       NOT NULL AUTO_INCREMENT COMMENT '主键',
    ast_id    VARCHAR(64)  NOT NULL COMMENT '资产ID',
    plan_id   VARCHAR(64)           COMMENT '退役计划ID',
    ctime     DATETIME              COMMENT '创建时间',
    PRIMARY KEY (id),
    INDEX idx_ast_id (ast_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='设备退役计划明细表';


-- ============================================================
-- Database: middleground_psr
-- 中台 - 电力系统资源 (Power System Resource)
-- ============================================================
CREATE DATABASE IF NOT EXISTS middleground_psr DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE middleground_psr;

-- 主变压器 PSR 表
CREATE TABLE t_psr_tf_maintransformer (
    psr_id        VARCHAR(64)  NOT NULL COMMENT 'PSR ID',
    voltage_level VARCHAR(20)           COMMENT '电压等级',
    capacity      DECIMAL(18,4)         COMMENT '容量(MVA)',
    PRIMARY KEY (psr_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='主变压器PSR表';

-- 变压器相位关联表
CREATE TABLE t_psr_tf_phase (
    id           BIGINT       NOT NULL AUTO_INCREMENT COMMENT '主键',
    ast_id       VARCHAR(64)  NOT NULL COMMENT '资产ID（关联 middleground_ast.t_ast_tf_maintransformer）',
    equip_psr_id VARCHAR(64)  NOT NULL COMMENT '设备PSR ID（关联 t_psr_tf_maintransformer）',
    PRIMARY KEY (id),
    INDEX idx_ast_id (ast_id),
    INDEX idx_equip_psr_id (equip_psr_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='变压器相位关联表';


-- ============================================================
-- Database: pms（生产作业管理 - 无前缀业务表）
-- ============================================================
CREATE DATABASE IF NOT EXISTS pms DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE pms;

-- ----------------------------------------------------------
-- 公共框架表
-- ----------------------------------------------------------

-- 系统参数组表
CREATE TABLE t_framework_system_param_group (
    obj_id     VARCHAR(64)  NOT NULL COMMENT '参数组ID',
    group_code VARCHAR(100) NOT NULL COMMENT '参数组编码',
    PRIMARY KEY (obj_id),
    INDEX idx_group_code (group_code)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='系统参数组表';

-- 系统参数表
CREATE TABLE t_framework_system_param (
    obj_id        VARCHAR(64)  NOT NULL COMMENT '参数ID',
    param_code    VARCHAR(100)          COMMENT '参数编码',
    param_name    VARCHAR(200)          COMMENT '参数名称',
    param_value   TEXT                  COMMENT '参数值',
    apply_province VARCHAR(64)          COMMENT '适用省份',
    apply_range   VARCHAR(100)          COMMENT '适用范围',
    param_group_id VARCHAR(64)          COMMENT '参数组ID（关联 t_framework_system_param_group）',
    is_deleted    TINYINT(1) DEFAULT 0  COMMENT '是否删除',
    PRIMARY KEY (obj_id),
    INDEX idx_param_code (param_code),
    INDEX idx_param_group_id (param_group_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='系统参数表';

-- 业务类型表
CREATE TABLE t_framework_business_type (
    obj_id             VARCHAR(64)  NOT NULL COMMENT '业务类型ID',
    business_type_code VARCHAR(100)          COMMENT '业务类型编码',
    business_type_name VARCHAR(200)          COMMENT '业务类型名称',
    create_user_id     VARCHAR(64)           COMMENT '创建人ID',
    create_user_name   VARCHAR(100)          COMMENT '创建人姓名',
    create_time        DATETIME              COMMENT '创建时间',
    parent_id          VARCHAR(64)           COMMENT '父类型ID',
    code_full_path     VARCHAR(500)          COMMENT '编码全路径',
    seq_no             INT                   COMMENT '排序号',
    PRIMARY KEY (obj_id),
    INDEX idx_code_full_path (code_full_path(255))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='业务类型配置表';

-- 特殊组织单元扩展属性表
CREATE TABLE t_framework_specialorg_unit_ext (
    id          BIGINT       NOT NULL AUTO_INCREMENT COMMENT '主键',
    org_id      VARCHAR(64)  NOT NULL COMMENT '组织ID',
    `key`       VARCHAR(100) NOT NULL COMMENT '扩展属性键',
    `value`     TEXT                  COMMENT '扩展属性值',
    source_type VARCHAR(50)           COMMENT '来源类型',
    create_time DATETIME              COMMENT '创建时间',
    create_by   VARCHAR(64)           COMMENT '创建人',
    update_time DATETIME              COMMENT '更新时间',
    update_by   VARCHAR(64)           COMMENT '更新人',
    PRIMARY KEY (id),
    INDEX idx_org_id (org_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='特殊组织单元扩展属性表';

-- 列配置表（用户自定义列）
CREATE TABLE t_framework_column_config (
    obj_id          VARCHAR(64)  NOT NULL COMMENT '配置ID',
    professional_kind VARCHAR(50)         COMMENT '专业类别',
    module          VARCHAR(100)          COMMENT '模块标识',
    user_id         VARCHAR(64)           COMMENT '用户ID',
    user_name       VARCHAR(100)          COMMENT '用户名称',
    ctime           DATETIME              COMMENT '创建时间',
    mtime           DATETIME              COMMENT '修改时间',
    deleted_state   TINYINT(1) DEFAULT 0  COMMENT '是否删除',
    PRIMARY KEY (obj_id),
    INDEX idx_user_module (user_id, module)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='列配置表';

-- 公共代码字典表（本地版，含扩展信息）
CREATE TABLE t_public_commom_dictionary (
    code_id                  VARCHAR(64)  NOT NULL COMMENT '代码ID',
    standtype_code           VARCHAR(100)          COMMENT '标准类型编码',
    code                     VARCHAR(100)          COMMENT '代码值',
    code_name                VARCHAR(200)          COMMENT '代码名称',
    remark                   VARCHAR(500)          COMMENT '备注',
    standtype_code_id        VARCHAR(64)           COMMENT '标准类型ID',
    sort                     INT                   COMMENT '排序',
    standtype_code_id_int    BIGINT                COMMENT '标准类型ID整型',
    standtype_code_int       BIGINT                COMMENT '标准类型编码整型',
    code_int                 BIGINT                COMMENT '代码值整型',
    is_enabled               TINYINT(1) DEFAULT 1  COMMENT '是否启用',
    serve_profession         VARCHAR(100)          COMMENT '适用专业',
    expand_information_one   VARCHAR(500)          COMMENT '扩展信息1',
    expand_information_tow   VARCHAR(500)          COMMENT '扩展信息2',
    expand_information_three VARCHAR(500)          COMMENT '扩展信息3',
    expand_information_four  VARCHAR(500)          COMMENT '扩展信息4',
    expand_information_five  VARCHAR(500)          COMMENT '扩展信息5',
    expand_information_six   VARCHAR(500)          COMMENT '扩展信息6',
    expand_information_seven VARCHAR(500)          COMMENT '扩展信息7',
    expand_information_eight VARCHAR(500)          COMMENT '扩展信息8',
    expand_information_nine  VARCHAR(500)          COMMENT '扩展信息9',
    expand_information_ten   VARCHAR(500)          COMMENT '扩展信息10',
    expand_information_eleven  VARCHAR(500)        COMMENT '扩展信息11',
    expand_information_twelve  VARCHAR(500)        COMMENT '扩展信息12',
    PRIMARY KEY (code_id),
    INDEX idx_standtype_code (standtype_code)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='公共代码字典表（本地版）';

-- 组织机构表（本地业务库引用版）
CREATE TABLE t_public_organization (
    org_id               VARCHAR(64)   NOT NULL COMMENT '组织机构ID',
    org_name             VARCHAR(200)           COMMENT '组织名称',
    org_nature           VARCHAR(50)            COMMENT '组织性质',
    org_code             VARCHAR(100)           COMMENT '组织编码',
    parent_id            VARCHAR(64)            COMMENT '上级ID',
    display_order        INT                    COMMENT '显示顺序',
    professional_nature  VARCHAR(50)            COMMENT '专业性质',
    manage_level         VARCHAR(20)            COMMENT '管理层级',
    org_short_name       VARCHAR(100)           COMMENT '简称',
    department_full_name VARCHAR(500)           COMMENT '部门全称',
    ctime                DATETIME               COMMENT '创建时间',
    syn_time             DATETIME               COMMENT '同步时间',
    repeal_time          DATETIME               COMMENT '撤销时间',
    branch_id            VARCHAR(64)            COMMENT '分支机构ID',
    province             VARCHAR(64)            COMMENT '所属省ID',
    city                 VARCHAR(64)            COMMENT '所属市ID',
    company              VARCHAR(64)            COMMENT '所属公司ID',
    reserved_id1         VARCHAR(64)            COMMENT '预留ID1',
    erp_org_type         VARCHAR(50)            COMMENT 'ERP组织类型',
    org_level            VARCHAR(20)            COMMENT '组织层级',
    full_path_name       VARCHAR(1000)          COMMENT '全路径名称',
    full_path_id         VARCHAR(500)           COMMENT '全路径ID',
    erp_code             VARCHAR(100)           COMMENT 'ERP编码',
    finance_code         VARCHAR(100)           COMMENT '财务编码',
    reserved_id3         VARCHAR(64)            COMMENT '预留ID3',
    maint_org            VARCHAR(64)            COMMENT '运维单位ID',
    org_full_name        VARCHAR(500)           COMMENT '全称',
    PRIMARY KEY (org_id),
    INDEX idx_full_path_id (full_path_id(255))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='组织机构表（pms本地）';

-- 用户表（本地业务库引用版）
CREATE TABLE t_public_user (
    user_id       VARCHAR(64)  NOT NULL COMMENT '用户ID',
    user_name     VARCHAR(100)          COMMENT '姓名',
    login_name    VARCHAR(100)          COMMENT '登录名',
    org_id        VARCHAR(64)           COMMENT '所属组织ID',
    title         VARCHAR(50)           COMMENT '职称',
    post          VARCHAR(50)           COMMENT '岗位',
    professional  VARCHAR(50)           COMMENT '专业',
    gender        VARCHAR(10)           COMMENT '性别',
    interphone    VARCHAR(50)           COMMENT '内线电话',
    department_id VARCHAR(64)           COMMENT '部门ID',
    dispatch_id   VARCHAR(64)           COMMENT '调度机构ID',
    dispatch_name VARCHAR(200)          COMMENT '调度机构名称',
    PRIMARY KEY (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='用户表（pms本地）';

-- 角色权限表
CREATE TABLE roles (
    role     VARCHAR(100) NOT NULL COMMENT '角色',
    username VARCHAR(100) NOT NULL COMMENT '用户名',
    PRIMARY KEY (role, username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='角色权限表';

-- ----------------------------------------------------------
-- 倒闸操作模块
-- ----------------------------------------------------------

-- 倒闸操作计划表
CREATE TABLE duty_reclosing_plan (
    obj_id          VARCHAR(64)  NOT NULL COMMENT '计划ID',
    del_flag        TINYINT(1) DEFAULT 0 COMMENT '删除标志',
    plan_category   VARCHAR(50)           COMMENT '计划类别',
    professional_kind VARCHAR(50)         COMMENT '专业类别',
    plan_start_time DATETIME              COMMENT '计划开始时间',
    plan_end_time   DATETIME              COMMENT '计划结束时间',
    plan_obj_id     VARCHAR(64)           COMMENT '关联对象ID（NULL 表示主计划）',
    plan_status     VARCHAR(20)           COMMENT '计划状态',
    PRIMARY KEY (obj_id),
    INDEX idx_plan_start_time (plan_start_time),
    INDEX idx_professional_kind (professional_kind)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='倒闸操作计划表';

-- 倒闸操作任务表
CREATE TABLE duty_reclosing_task (
    obj_id          VARCHAR(64)  NOT NULL COMMENT '任务ID',
    del_flag        TINYINT(1) DEFAULT 0 COMMENT '删除标志',
    professional_kind VARCHAR(50)         COMMENT '专业类别',
    oper_start_time DATETIME              COMMENT '操作开始时间',
    oper_end_time   DATETIME              COMMENT '操作结束时间',
    parent_obj_id   VARCHAR(64)           COMMENT '父任务ID（NULL 表示顶层任务）',
    PRIMARY KEY (obj_id),
    INDEX idx_oper_start_time (oper_start_time)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='倒闸操作任务表';

-- 倒闸操作方案表
CREATE TABLE duty_reclosing_scheme (
    obj_id          VARCHAR(64)  NOT NULL COMMENT '方案ID',
    deleted_state   TINYINT(1) DEFAULT 0 COMMENT '删除状态',
    oper_start_time DATETIME              COMMENT '操作开始时间',
    oper_end_time   DATETIME              COMMENT '操作结束时间',
    professional_kind VARCHAR(50)         COMMENT '专业类别（支持多值 IN 查询）',
    PRIMARY KEY (obj_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='倒闸操作方案表';

-- 监控处置记录表
CREATE TABLE tf_monitor_dispose_record (
    id                    VARCHAR(64)  NOT NULL COMMENT '记录ID',
    tf_monitor_main_log_id  VARCHAR(64)          COMMENT '主监控日志ID',
    tf_monitor_child_log_id VARCHAR(64)          COMMENT '子监控日志ID',
    deal_time             DATETIME              COMMENT '处置时间',
    dealer_id             VARCHAR(64)           COMMENT '处置人ID',
    dealer_name           VARCHAR(100)          COMMENT '处置人姓名',
    deal_content          TEXT                  COMMENT '处置内容',
    tag                   VARCHAR(100)          COMMENT '标签',
    PRIMARY KEY (id),
    INDEX idx_main_log_id (tf_monitor_main_log_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='监控处置记录表';

-- ----------------------------------------------------------
-- 检修计划/作业模块
-- ----------------------------------------------------------

-- 停电申请表
CREATE TABLE outage_apply (
    obj_id              VARCHAR(64)  NOT NULL COMMENT '停电申请ID',
    repair_plan_id      VARCHAR(64)           COMMENT '检修计划ID',
    state               VARCHAR(20)           COMMENT '申请状态',
    apply_work_stime    DATETIME              COMMENT '申请作业开始时间',
    apply_work_etime    DATETIME              COMMENT '申请作业结束时间',
    apply_outg_stime    DATETIME              COMMENT '申请停电开始时间',
    apply_outg_etime    DATETIME              COMMENT '申请停电结束时间',
    outg_type           VARCHAR(50)           COMMENT '停电类型',
    declare_categ       VARCHAR(50)           COMMENT '申报类别',
    disp_work_nature    VARCHAR(50)           COMMENT '调度工作性质',
    work_content        TEXT                  COMMENT '工作内容',
    power_contact_id    VARCHAR(64)           COMMENT '用电联系人ID',
    power_contact_name  VARCHAR(100)          COMMENT '用电联系人姓名',
    power_contact_tel   VARCHAR(50)           COMMENT '用电联系人电话',
    scope_type          VARCHAR(50)           COMMENT '范围类型',
    construct_org_name  VARCHAR(200)          COMMENT '施工单位名称',
    outg_plan_state     VARCHAR(20)           COMMENT '停电计划状态',
    outg_apply_no       VARCHAR(100)          COMMENT '停电申请编号',
    applicante_id       VARCHAR(64)           COMMENT '申请人ID',
    applicante_name     VARCHAR(100)          COMMENT '申请人姓名',
    creater_dept_id     VARCHAR(64)           COMMENT '创建部门ID',
    creater_dept_name   VARCHAR(200)          COMMENT '创建部门名称',
    mtime               DATETIME              COMMENT '修改时间',
    deleted_state       TINYINT(1) DEFAULT 0  COMMENT '删除状态',
    professional_kind   VARCHAR(50)           COMMENT '专业类别',
    PRIMARY KEY (obj_id),
    INDEX idx_repair_plan_id (repair_plan_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='停电申请表';

-- 检修作业工单表
CREATE TABLE repair_work (
    obj_id             VARCHAR(64)  NOT NULL COMMENT '工单ID',
    work_no            VARCHAR(100)          COMMENT '工单编号',
    work_title         VARCHAR(500)          COMMENT '工单标题',
    state              VARCHAR(20)           COMMENT '工单状态',
    remark             TEXT                  COMMENT '备注',
    creater_id         VARCHAR(64)           COMMENT '创建人ID',
    creater_name       VARCHAR(100)          COMMENT '创建人姓名',
    ctime              DATETIME              COMMENT '创建时间',
    editor_id          VARCHAR(64)           COMMENT '修改人ID',
    editor_name        VARCHAR(100)          COMMENT '修改人姓名',
    mtime              DATETIME              COMMENT '修改时间',
    maint_crew_id      VARCHAR(64)           COMMENT '运维班组ID',
    maint_crew_name    VARCHAR(200)          COMMENT '运维班组名称',
    maintainer_id      VARCHAR(64)           COMMENT '维护人员ID',
    maintainer_name    VARCHAR(100)          COMMENT '维护人员姓名',
    city_org_id        VARCHAR(64)           COMMENT '地市组织ID',
    city_org_name      VARCHAR(200)          COMMENT '地市组织名称',
    data_source_type   VARCHAR(50)           COMMENT '数据来源类型',
    data_source_name   VARCHAR(200)          COMMENT '数据来源名称',
    app_id             VARCHAR(64)           COMMENT '应用ID',
    app_name           VARCHAR(200)          COMMENT '应用名称',
    deleted_state      TINYINT(1) DEFAULT 0  COMMENT '删除状态',
    repair_business_id VARCHAR(64)           COMMENT '检修业务ID',
    ticket_type        VARCHAR(50)           COMMENT '工票类型',
    work_crew_id       VARCHAR(64)           COMMENT '工作班组ID',
    work_crew_name     VARCHAR(200)          COMMENT '工作班组名称',
    plan_stime         DATETIME              COMMENT '计划开始时间',
    plan_etime         DATETIME              COMMENT '计划结束时间',
    professional_kind  VARCHAR(50)           COMMENT '专业类别',
    PRIMARY KEY (obj_id),
    INDEX idx_city_org_id (city_org_id),
    INDEX idx_plan_stime (plan_stime)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='检修作业工单表';

-- 检修方案表
CREATE TABLE repair_scheme (
    obj_id          VARCHAR(64)  NOT NULL COMMENT '方案ID',
    professional_kind VARCHAR(50)          COMMENT '专业类别',
    scheme_type     VARCHAR(50)           COMMENT '方案类型',
    plan_stime      DATETIME              COMMENT '计划开始时间',
    plan_etime      DATETIME              COMMENT '计划结束时间',
    deleted_state   TINYINT(1) DEFAULT 0  COMMENT '删除状态',
    PRIMARY KEY (obj_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='检修方案表';

-- 检修记录表
CREATE TABLE repair_record (
    obj_id          VARCHAR(64)  NOT NULL COMMENT '记录ID',
    deleted_state   TINYINT(1) DEFAULT 0  COMMENT '删除状态',
    ctime           DATETIME              COMMENT '创建时间',
    professional_kind VARCHAR(50)         COMMENT '专业类别',
    PRIMARY KEY (obj_id),
    INDEX idx_ctime (ctime)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='检修记录表';

-- 检修工作票作业规程文件表
CREATE TABLE repair_proc_doc (
    obj_id            VARCHAR(64)  NOT NULL COMMENT '规程文件ID',
    professional_kind VARCHAR(50)           COMMENT '专业类别',
    proc_doc_type     VARCHAR(50)           COMMENT '规程文件类型',
    work_risk_level_code VARCHAR(20)        COMMENT '作业风险等级',
    deleted_state     TINYINT(1) DEFAULT 0  COMMENT '删除状态',
    city_org_id       VARCHAR(64)           COMMENT '地市组织ID',
    city_org_name     VARCHAR(200)          COMMENT '地市组织名称',
    ctime             DATETIME              COMMENT '创建时间',
    PRIMARY KEY (obj_id),
    INDEX idx_city_org_id (city_org_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='检修作业规程文件表';

-- 日工作计划表（工作票）
CREATE TABLE workticket_daily_workplan (
    obj_id            VARCHAR(64)  NOT NULL COMMENT '日计划ID',
    is_deleted        TINYINT(1) DEFAULT 0  COMMENT '是否删除',
    professional_kind VARCHAR(50)           COMMENT '专业类别',
    plan_start_time   DATETIME              COMMENT '计划开始时间',
    plan_end_time     DATETIME              COMMENT '计划结束时间',
    PRIMARY KEY (obj_id),
    INDEX idx_plan_start_time (plan_start_time)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='日工作计划表';

-- 值班签到表
CREATE TABLE repair_duty_sign (
    obj_id                     VARCHAR(64)  NOT NULL COMMENT '签到ID',
    workticket_daily_workplan_id VARCHAR(64)          COMMENT '日工作计划ID',
    duty_person_id             VARCHAR(64)           COMMENT '值班人员ID',
    duty_person_name           VARCHAR(100)          COMMENT '值班人员姓名',
    duty_person_level          VARCHAR(50)           COMMENT '值班人员级别',
    duty_person_type           VARCHAR(50)           COMMENT '值班人员类型',
    sign_in_info               VARCHAR(200)          COMMENT '签到信息',
    deleted_state              TINYINT(1) DEFAULT 0  COMMENT '删除状态',
    relate_busi_type           VARCHAR(50)           COMMENT '关联业务类型',
    PRIMARY KEY (obj_id),
    INDEX idx_workplan_id (workticket_daily_workplan_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='值班签到表';

-- 工作票汇总表
CREATE TABLE work_ticket_public (
    obj_id          VARCHAR(64)  NOT NULL COMMENT '工票ID',
    city_org_id     VARCHAR(64)           COMMENT '地市组织ID',
    city_org_name   VARCHAR(200)          COMMENT '地市组织名称',
    source_way      VARCHAR(50)           COMMENT '生成来源（AI/人工等）',
    ticket_state    VARCHAR(20)           COMMENT '工票状态',
    evaluate_result VARCHAR(200)          COMMENT '评价结果',
    professional_kind VARCHAR(50)         COMMENT '专业类别',
    deleted_state   TINYINT(1) DEFAULT 0  COMMENT '删除状态',
    PRIMARY KEY (obj_id),
    INDEX idx_city_org_id (city_org_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='工作票汇总表';

-- ----------------------------------------------------------
-- 巡视/特巡作业模块
-- ----------------------------------------------------------

-- 巡视工作表
CREATE TABLE patrol_work (
    obj_id             VARCHAR(64)  NOT NULL COMMENT '巡视ID',
    patrol_type        VARCHAR(50)           COMMENT '巡视类型',
    patrol_work_state  VARCHAR(20)           COMMENT '巡视状态',
    deleted_state      TINYINT(1) DEFAULT 0  COMMENT '删除状态',
    professional_kind  VARCHAR(50)           COMMENT '专业类别',
    plan_stime         DATETIME              COMMENT '计划开始时间',
    plan_etime         DATETIME              COMMENT '计划结束时间',
    PRIMARY KEY (obj_id),
    INDEX idx_plan_stime (plan_stime)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='巡视工作表';

-- 巡视工作任务表
CREATE TABLE patrol_work_task (
    obj_id                 VARCHAR(64)  NOT NULL COMMENT '巡视任务ID',
    deleted_state          TINYINT(1) DEFAULT 0 COMMENT '删除状态',
    professional_kind      VARCHAR(50)          COMMENT '专业类别',
    patrol_type            VARCHAR(50)          COMMENT '巡视类型',
    patrol_work_task_state VARCHAR(20)          COMMENT '任务状态',
    work_task_stime        DATETIME             COMMENT '任务开始时间',
    work_task_etime        DATETIME             COMMENT '任务结束时间',
    PRIMARY KEY (obj_id),
    INDEX idx_work_task_stime (work_task_stime)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='巡视工作任务表';

-- ----------------------------------------------------------
-- 现场勘察模块
-- ----------------------------------------------------------

-- 现场勘察工单表
CREATE TABLE site_survey_work (
    obj_id              VARCHAR(64)  NOT NULL COMMENT '勘察ID',
    professional_kind   VARCHAR(50)           COMMENT '专业类别',
    plan_stime          DATETIME              COMMENT '计划开始时间',
    plan_etime          DATETIME              COMMENT '计划结束时间',
    deleted_state       TINYINT(1) DEFAULT 0  COMMENT '删除状态',
    site_survey_status  VARCHAR(20)           COMMENT '勘察状态',
    PRIMARY KEY (obj_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='现场勘察工单表';

-- ----------------------------------------------------------
-- 检测试验模块
-- ----------------------------------------------------------

-- 试验数据记录表
CREATE TABLE test_data_record (
    obj_id            VARCHAR(64)  NOT NULL COMMENT '试验记录ID',
    is_deleted        TINYINT(1) DEFAULT 0  COMMENT '是否删除',
    professional_kind VARCHAR(50)           COMMENT '专业类别',
    state             VARCHAR(20)           COMMENT '记录状态',
    major_code        VARCHAR(50)           COMMENT '专业编码',
    test_nature_code  VARCHAR(50)           COMMENT '试验性质编码',
    ctime             DATETIME              COMMENT '创建时间',
    PRIMARY KEY (obj_id),
    INDEX idx_ctime (ctime)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='试验数据记录表';

-- 试验数据设备关联表
CREATE TABLE test_data_equip (
    id                 BIGINT       NOT NULL AUTO_INCREMENT COMMENT '主键',
    test_data_record_id VARCHAR(64) NOT NULL COMMENT '试验记录ID',
    ast_id             VARCHAR(64)           COMMENT '资产ID',
    PRIMARY KEY (id),
    INDEX idx_test_data_record_id (test_data_record_id),
    INDEX idx_ast_id (ast_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='试验数据设备关联表';

-- 试验项目位置扩展表
CREATE TABLE test_item_pos_expand (
    id                  BIGINT       NOT NULL AUTO_INCREMENT COMMENT '主键',
    test_data_record_id VARCHAR(64)  NOT NULL COMMENT '试验记录ID',
    equip_pos_state     VARCHAR(20)           COMMENT '设备位置状态',
    ast_id              VARCHAR(64)           COMMENT '资产ID',
    PRIMARY KEY (id),
    INDEX idx_test_data_record_id (test_data_record_id),
    INDEX idx_ast_id (ast_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='试验项目位置扩展表';

-- 检测试验计划表（周期性）
CREATE TABLE test_inspection_plan (
    obj_id            VARCHAR(64)  NOT NULL COMMENT '计划ID',
    is_deleted        TINYINT(1) DEFAULT 0  COMMENT '是否删除',
    professional_kind VARCHAR(50)           COMMENT '专业类别',
    plan_state        VARCHAR(20)           COMMENT '计划状态',
    plan_type         VARCHAR(50)           COMMENT '计划类型',
    ctime             DATETIME              COMMENT '创建时间',
    PRIMARY KEY (obj_id),
    INDEX idx_ctime (ctime)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='检测试验计划表';

-- 检测周期计划表
CREATE TABLE test_period_plan (
    obj_id            VARCHAR(64)  NOT NULL COMMENT '周期计划ID',
    is_deleted        TINYINT(1) DEFAULT 0  COMMENT '是否删除',
    professional_kind VARCHAR(50)           COMMENT '专业类别',
    plan_state        VARCHAR(20)           COMMENT '计划状态',
    ctime             DATETIME              COMMENT '创建时间',
    PRIMARY KEY (obj_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='检测周期计划表';

-- ----------------------------------------------------------
-- 检修设备清单模块
-- ----------------------------------------------------------

-- 检修项目设备清单表（tro = 技改/大修）
CREATE TABLE tro_project_equip_pl (
    id                   BIGINT       NOT NULL AUTO_INCREMENT COMMENT '主键',
    prj_id               VARCHAR(64)  NOT NULL COMMENT '项目ID',
    station_line_type    VARCHAR(50)           COMMENT '站线类型',
    station_line_id      VARCHAR(64)           COMMENT '站线ID',
    station_line_name    VARCHAR(200)          COMMENT '站线名称',
    station_line_volt_level VARCHAR(20)        COMMENT '站线电压等级',
    equip_type           VARCHAR(50)           COMMENT '设备类型',
    ast_id               VARCHAR(64)           COMMENT '资产ID',
    psr_id               VARCHAR(64)           COMMENT 'PSR ID',
    equip_name           VARCHAR(200)          COMMENT '设备名称',
    voltage_level        VARCHAR(20)           COMMENT '电压等级',
    bay                  VARCHAR(64)           COMMENT '间隔ID',
    bay_name             VARCHAR(200)          COMMENT '间隔名称',
    operate_date         DATE                  COMMENT '投运日期',
    PRIMARY KEY (id),
    INDEX idx_prj_id (prj_id),
    INDEX idx_ast_id (ast_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='检修项目设备清单表';

-- ----------------------------------------------------------
-- 验收管理模块
-- ----------------------------------------------------------

-- 验收项目表
CREATE TABLE paw_project (
    obj_id          VARCHAR(64)  NOT NULL COMMENT '验收项目ID',
    is_deleted      TINYINT(1) DEFAULT 0  COMMENT '是否删除',
    professional_kind VARCHAR(50)         COMMENT '专业类别',
    org_path_id     VARCHAR(500)          COMMENT '组织路径ID（用于 LIKE 查询）',
    project_type    VARCHAR(50)           COMMENT '项目类型',
    PRIMARY KEY (obj_id),
    INDEX idx_org_path_id (org_path_id(255))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='验收项目表';

-- 验收问题信息表
CREATE TABLE paw_accp_question_info (
    obj_id                 VARCHAR(64)  NOT NULL COMMENT '问题ID',
    accp_item_name         VARCHAR(200)          COMMENT '验收项名称',
    paw_accp_work_id       VARCHAR(64)           COMMENT '验收工作ID',
    problem_name           VARCHAR(500)          COMMENT '问题名称',
    equip_type_code        VARCHAR(50)           COMMENT '设备类型编码',
    equip_type_name        VARCHAR(200)          COMMENT '设备类型名称',
    creater_id             VARCHAR(64)           COMMENT '创建人ID',
    creater_name           VARCHAR(100)          COMMENT '创建人姓名',
    ctime                  DATETIME              COMMENT '创建时间',
    mtime                  DATETIME              COMMENT '修改时间',
    is_deleted             TINYINT(1) DEFAULT 0  COMMENT '是否删除',
    acceptor_name          VARCHAR(100)          COMMENT '验收人姓名',
    problem_type           VARCHAR(50)           COMMENT '问题类型',
    problem_type_name      VARCHAR(200)          COMMENT '问题类型名称',
    is_remediated          VARCHAR(20)           COMMENT '是否整改',
    is_significant_problem TINYINT(1)            COMMENT '是否重要问题',
    problem_desc           TEXT                  COMMENT '问题描述',
    equip_id               VARCHAR(64)           COMMENT '设备ID',
    PRIMARY KEY (obj_id),
    INDEX idx_paw_accp_work_id (paw_accp_work_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='验收问题信息表';

-- ----------------------------------------------------------
-- 带电作业模块
-- ----------------------------------------------------------

-- 带电作业计划主表
CREATE TABLE live_plan_tr (
    obj_id         VARCHAR(64)  NOT NULL COMMENT '计划ID',
    is_deleted     TINYINT(1) DEFAULT 0 COMMENT '是否删除',
    plan_name      VARCHAR(500)          COMMENT '计划名称',
    professional_kind VARCHAR(50)        COMMENT '专业类别',
    PRIMARY KEY (obj_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='带电作业计划主表';

-- 带电作业任务表
CREATE TABLE live_worktask_tr (
    obj_id          VARCHAR(64)  NOT NULL COMMENT '任务ID',
    task_source_id  VARCHAR(64)           COMMENT '来源计划ID（关联 live_plan_tr）',
    is_deleted      TINYINT(1) DEFAULT 0  COMMENT '是否删除',
    is_live_condition TINYINT(1)          COMMENT '是否满足带电作业条件',
    state           VARCHAR(20)           COMMENT '任务状态',
    plan_task_date  DATE                  COMMENT '计划作业日期',
    PRIMARY KEY (obj_id),
    INDEX idx_task_source_id (task_source_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='带电作业任务表';

-- ----------------------------------------------------------
-- 隐患与缺陷模块
-- ----------------------------------------------------------

-- 隐患/缺陷记录表
CREATE TABLE ast_hazard_record (
    obj_id             VARCHAR(64)  NOT NULL COMMENT '记录ID',
    deleted_state      TINYINT(1) DEFAULT 0 COMMENT '删除状态',
    state              VARCHAR(20)           COMMENT '记录状态',
    equipment_category VARCHAR(50)           COMMENT '设备大类',
    professional_kind  VARCHAR(50)           COMMENT '专业类别',
    ctime              DATETIME              COMMENT '创建时间',
    PRIMARY KEY (obj_id),
    INDEX idx_state (state)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='隐患/缺陷记录表';

-- ----------------------------------------------------------
-- 故障分析模块
-- ----------------------------------------------------------

-- 故障停电分析表
CREATE TABLE fault_outage_analysis (
    obj_id           VARCHAR(64)  NOT NULL COMMENT '分析记录ID',
    fault_rec_id     VARCHAR(64)  NOT NULL COMMENT '故障记录ID',
    outage_equip_name VARCHAR(200)          COMMENT '停电设备名称',
    PRIMARY KEY (obj_id),
    INDEX idx_fault_rec_id (fault_rec_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='故障停电分析表';
